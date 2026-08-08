#include "external_monitor_probe.h"

#include <cassert>
#include <stdexcept>

using external_monitor::ProbeSchedule;

int main() {
    ProbeSchedule first;
    ProbeSchedule same;
    ProbeSchedule other;
    first.Initialize("aa:bb:cc:dd:ee:ff", 1000000);
    same.Initialize("aa:bb:cc:dd:ee:ff", 1000000);
    other.Initialize("aa:bb:cc:dd:ee:00", 1000000);
    assert(first.jitter_seconds() == same.jitter_seconds());
    assert(first.next_probe_us() == same.next_probe_us());
    assert(first.jitter_seconds() >= 0);
    assert(first.jitter_seconds() <= ProbeSchedule::kMaximumInitialJitterSeconds);
    assert(!first.Ready(first.next_probe_us() - 1));
    assert(first.Ready(first.next_probe_us()));

    const int64_t now = 100000000;
    first.OnFailure(now);
    assert(first.next_probe_us() == now + 300LL * 1000000);
    assert(first.failure_delay_seconds() == 600);
    first.OnFailure(now);
    assert(first.next_probe_us() == now + 600LL * 1000000);
    assert(first.failure_delay_seconds() == 1200);
    first.OnFailure(now);
    assert(first.failure_delay_seconds() == 1800);
    first.OnFailure(now);
    assert(first.next_probe_us() == now + 1800LL * 1000000);
    assert(first.failure_delay_seconds() == 1800);

    first.OnSuccess(now, 300);
    assert(first.next_probe_us() == now + 300LL * 1000000);
    assert(first.failure_delay_seconds() == 300);
    assert(external_monitor::IsSupportedTopic("weather"));
    assert(external_monitor::IsSupportedTopic("news"));
    assert(!external_monitor::IsSupportedTopic("sports"));
    assert(external_monitor::IsSupportedPriority("critical"));
    assert(!external_monitor::IsSupportedPriority("urgent"));

    std::time_t manager_time = 0;
    assert(external_monitor::ParseManagerDateTime("2026-08-09 08:00:00", manager_time));
    assert(manager_time == 1786233600);  // 2026-08-09 00:00:00 UTC
    assert(external_monitor::ParseManagerDateTime("2024-02-29 23:59:59", manager_time));
    assert(!external_monitor::ParseManagerDateTime("2023-02-29 23:59:59", manager_time));
    assert(!external_monitor::ParseManagerDateTime("2026-13-01 00:00:00", manager_time));
    assert(!external_monitor::ParseManagerDateTime("2026-08-09T08:00:00+08:00", manager_time));

    bool empty_rejected = false;
    try {
        ProbeSchedule invalid;
        invalid.Initialize("", now);
    } catch (const std::invalid_argument&) {
        empty_rejected = true;
    }
    assert(empty_rejected);

    bool retry_rejected = false;
    try {
        first.OnSuccess(now, 0);
    } catch (const std::invalid_argument&) {
        retry_rejected = true;
    }
    assert(retry_rejected);
    (void)other;
    return 0;
}
