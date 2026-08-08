#ifndef PROACTIVE_MANAGER_H
#define PROACTIVE_MANAGER_H

#include <cstdint>
#include <ctime>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace proactive {

enum class Mode { kConservative, kActive, kAggressive, kTodaySilent };
enum class Priority { kLow, kNormal, kHigh, kCritical };
enum class Severity { kInfo, kWarning, kCritical };

struct Event {
    std::string event_id;
    std::string topic;
    Priority priority = Priority::kNormal;
    std::string reason;
    std::time_t created_at = 0;
    std::time_t expires_at = 0;
    std::string dedupe_key;
    bool requires_response = false;
    std::map<std::string, std::string> metadata;
};

struct Config {
    Mode mode = Mode::kAggressive;
    int daily_limit = 0;
    std::optional<int> quiet_start;
    std::optional<int> quiet_end;
    std::set<std::string> allowed_topics;
    std::set<std::string> blocked_topics;
    Mode mode_before_silent = Mode::kAggressive;
    int previous_daily_limit = 0;
    int silent_date = 0;
};

struct RuntimeState {
    int budget_date = 0;
    int delivered_today = 0;
    std::map<std::string, std::time_t> last_delivered;
};

class Manager {
public:
    static constexpr int kCooldownSeconds = 30 * 60;

    const Config& config() const { return config_; }
    const RuntimeState& state() const { return state_; }
    void Restore(Config config, RuntimeState state);
    void Configure(Mode mode, std::optional<int> daily_limit,
                   std::optional<int> quiet_start, std::optional<int> quiet_end);
    void MuteToday(std::time_t now, bool time_valid);
    void AllowTopic(const std::string& topic);
    void BlockTopic(const std::string& topic);
    bool ShouldDeliver(const Event& event, std::time_t now, bool time_valid);
    void RecordDelivered(const Event& event, std::time_t now, bool time_valid);

    static Mode ParseMode(const std::string& value);
    static const char* ModeName(Mode mode);
    static int DefaultLimit(Mode mode);
    static int ParseClock(const std::string& value);
    static std::string FormatClock(int minutes);

private:
    void TrimCooldowns(std::time_t now);
    void RefreshDate(std::time_t now, bool time_valid);
    bool IsQuiet(std::time_t now) const;
    static bool IsCritical(const Event& event);

    Config config_;
    RuntimeState state_;
};

struct FollowUp {
    uint32_t source_id = 0;
    std::string label;
    std::time_t source_triggered_at = 0;
    std::time_t due_at = 0;
    std::time_t expires_at = 0;
    bool asked = false;
};

class FollowUpStore {
public:
    static constexpr size_t kMaxItems = 4;
    static constexpr int kDefaultDelaySeconds = 10 * 60;
    static constexpr int kLateGraceSeconds = 5 * 60;
    const std::vector<FollowUp>& items() const { return items_; }
    void Restore(std::vector<FollowUp> items);
    void Schedule(uint32_t source_id, const std::string& label,
                  std::time_t source_triggered_at, std::time_t scheduled_at);
    std::vector<FollowUp> PendingDue(std::time_t now);
    void MarkAsked(uint32_t source_id, std::time_t due_at);
    FollowUp CompleteRecent(std::time_t now);
    FollowUp DelayRecent(int minutes, std::time_t now);
    FollowUp DismissRecent(std::time_t now);
    size_t DropExpired(std::time_t now);

private:
    size_t FindRecent(std::time_t now) const;
    std::vector<FollowUp> items_;
};

struct HealthEvent {
    Event event;
    std::string kind;
    Severity severity = Severity::kInfo;
    std::time_t occurred_at = 0;
    bool recovered = false;
    std::map<std::string, std::string> details;
};

class HealthTracker {
public:
    static constexpr int kCooldownSeconds = 30 * 60;
    struct Record {
        bool active = false;
        std::time_t last_changed_at = 0;
        std::string dedupe_key;
    };
    const std::map<std::string, Record>& records() const { return records_; }
    void Restore(std::map<std::string, Record> records);
    std::optional<HealthEvent> Raise(const std::string& kind, Severity severity,
                                     std::time_t now,
                                     std::map<std::string, std::string> details);
    std::optional<HealthEvent> Recover(const std::string& kind, std::time_t now);
    static const char* SeverityName(Severity severity);
    static bool IsSupportedKind(const std::string& kind);

private:
    std::map<std::string, Record> records_;
};

class DurableQueue {
public:
    static constexpr size_t kMaxItems = 8;
    const std::vector<Event>& items() const { return items_; }
    void Restore(std::vector<Event> items);
    std::optional<Event> Push(Event event);
    std::optional<Event> PopNext(std::time_t now);
    size_t RemoveByDedupeKey(const std::string& dedupe_key);
    size_t DropExpired(std::time_t now);

private:
    std::vector<Event> items_;
};

class RetryBackoff {
public:
    bool Ready(int64_t now_us) const { return now_us >= retry_after_us_; }
    void OnFailure(int64_t now_us);
    void OnSuccess();
    int delay_seconds() const { return delay_seconds_; }
    int64_t retry_after_us() const { return retry_after_us_; }

private:
    int delay_seconds_ = 5;
    int64_t retry_after_us_ = 0;
};

class StateCodec {
public:
    static std::vector<uint8_t> Compress(const std::string& input);
    static std::string Decompress(const uint8_t* data, size_t size, size_t max_output);
};

}  // namespace proactive

#endif
