#include "external_monitor_probe.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace external_monitor {
namespace {

uint32_t Fnv1a(const std::string& value) {
    uint32_t hash = 2166136261u;
    for (unsigned char byte : value) {
        hash = (hash ^ byte) * 16777619u;
    }
    return hash;
}

}  // namespace

void ProbeSchedule::Initialize(const std::string& device_id, int64_t now_us) {
    if (device_id.empty()) {
        throw std::invalid_argument("device_id不能为空");
    }
    jitter_seconds_ = static_cast<int>(Fnv1a(device_id) %
                                       (kMaximumInitialJitterSeconds + 1));
    failure_delay_seconds_ = kRegularIntervalSeconds;
    next_probe_us_ = now_us + static_cast<int64_t>(jitter_seconds_) * 1000000;
}

bool ProbeSchedule::Ready(int64_t now_us) const {
    return next_probe_us_ > 0 && now_us >= next_probe_us_;
}

void ProbeSchedule::OnSuccess(int64_t now_us, int retry_after_seconds) {
    if (retry_after_seconds < 1 || retry_after_seconds > kMaximumFailureDelaySeconds) {
        throw std::invalid_argument("retry_after_seconds超出范围");
    }
    failure_delay_seconds_ = kRegularIntervalSeconds;
    next_probe_us_ = now_us + static_cast<int64_t>(retry_after_seconds) * 1000000;
}

void ProbeSchedule::OnFailure(int64_t now_us) {
    next_probe_us_ = now_us + static_cast<int64_t>(failure_delay_seconds_) * 1000000;
    failure_delay_seconds_ = std::min(failure_delay_seconds_ * 2,
                                     kMaximumFailureDelaySeconds);
}

bool IsSupportedTopic(const std::string& topic) {
    return topic == "weather" || topic == "news";
}

bool IsSupportedPriority(const std::string& priority) {
    return priority == "low" || priority == "normal" || priority == "high" ||
           priority == "critical";
}

bool ParseManagerDateTime(const std::string& value, std::time_t& timestamp) {
    if (value.size() != 19) return false;
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    char tail = 0;
    if (std::sscanf(value.c_str(), "%d-%d-%d %d:%d:%d%c", &year, &month, &day,
                    &hour, &minute, &second, &tail) != 6) {
        return false;
    }
    const bool leap_year = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    constexpr int kMonthDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (year < 1970 || month < 1 || month > 12 || day < 1 ||
        day > kMonthDays[month - 1] + (month == 2 && leap_year ? 1 : 0) ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
        second < 0 || second > 59) {
        return false;
    }

    // manager-api serializes this legacy field in fixed GMT+8. Convert it to
    // Unix time without consulting the device's configurable local timezone.
    constexpr int kManagerTimezoneOffsetSeconds = 8 * 60 * 60;
    const int adjusted_year = year - (month <= 2 ? 1 : 0);
    const int era = (adjusted_year >= 0 ? adjusted_year : adjusted_year - 399) / 400;
    const unsigned year_of_era = static_cast<unsigned>(adjusted_year - era * 400);
    const unsigned adjusted_month = static_cast<unsigned>(month + (month > 2 ? -3 : 9));
    const unsigned day_of_year = (153 * adjusted_month + 2) / 5 +
                                 static_cast<unsigned>(day - 1);
    const unsigned day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 +
                                day_of_year;
    const int64_t days_since_epoch = static_cast<int64_t>(era) * 146097 +
                                     static_cast<int64_t>(day_of_era) - 719468;
    const int64_t unix_seconds = days_since_epoch * 86400 + hour * 3600 + minute * 60 +
                                 second - kManagerTimezoneOffsetSeconds;
    if (unix_seconds <= 0 ||
        static_cast<uint64_t>(unix_seconds) >
            static_cast<uint64_t>(std::numeric_limits<std::time_t>::max())) {
        return false;
    }
    timestamp = static_cast<std::time_t>(unix_seconds);
    return true;
}

bool ReadBoundedResponseBody(size_t declared_length, size_t maximum_length,
                             const std::function<int(char*, size_t)>& read,
                             std::string& body, std::string& error) {
    body.clear();
    error.clear();
    if (maximum_length == 0 || declared_length > maximum_length) {
        error = "pending响应超过大小限制";
        return false;
    }

    if (declared_length > 0) {
        body.resize(declared_length);
        size_t total_read = 0;
        while (total_read < declared_length) {
            const size_t remaining = declared_length - total_read;
            const int count = read(body.data() + total_read, remaining);
            if (count < 0) {
                body.clear();
                error = "pending响应读取失败";
                return false;
            }
            if (count == 0 || static_cast<size_t>(count) > remaining) {
                body.clear();
                error = "pending响应读取不完整";
                return false;
            }
            total_read += static_cast<size_t>(count);
        }
        return true;
    }

    std::array<char, 256> buffer;
    while (true) {
        const size_t remaining = maximum_length - body.size();
        const size_t request_size = remaining < buffer.size() ? remaining + 1 : buffer.size();
        const int count = read(buffer.data(), request_size);
        if (count < 0 || static_cast<size_t>(count) > request_size) {
            body.clear();
            error = "pending响应读取失败";
            return false;
        }
        if (count == 0) {
            if (body.empty()) {
                error = "pending响应为空";
                return false;
            }
            return true;
        }
        if (static_cast<size_t>(count) > remaining) {
            body.clear();
            error = "pending响应超过大小限制";
            return false;
        }
        body.append(buffer.data(), static_cast<size_t>(count));
    }
}

}  // namespace external_monitor
