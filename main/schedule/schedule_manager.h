#ifndef SCHEDULE_MANAGER_H
#define SCHEDULE_MANAGER_H

#include <cstdint>
#include <ctime>
#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace schedule {

bool ShouldRestoreTemporaryVolume(uint32_t start_revision, uint32_t current_revision);

enum class Kind { kAlarm, kReminder };
enum class KindFilter { kAll, kAlarm, kReminder };
enum class Repeat { kOnce, kDaily, kWeekdays, kWeekends, kWeekly };
enum class ReminderDeliveryState { kInactive, kWaitingForCue, kWaitingForTts, kSpeaking };

struct Task {
    uint32_t id = 0;
    Kind kind = Kind::kReminder;
    Repeat repeat = Repeat::kOnce;
    std::string label;
    std::time_t trigger_at = 0;
    std::vector<int> weekdays;
};

struct CreateRequest {
    Kind kind = Kind::kReminder;
    Repeat repeat = Repeat::kOnce;
    std::string label;
    std::time_t trigger_at = 0;
    int delay_seconds = 0;
    std::vector<int> weekdays;
};

struct TickResult {
    std::vector<Task> triggered;
    std::vector<Task> missed;
    bool changed = false;
};

class Manager {
public:
    static constexpr size_t kMaxTasks = 16;
    static constexpr int kOnceRecoveryWindowSeconds = 300;

    const std::vector<Task>& tasks() const { return tasks_; }
    uint32_t next_id() const { return next_id_; }
    void Restore(std::vector<Task> tasks, uint32_t next_id);
    Task Create(const CreateRequest& request, std::time_t now);
    Task Snooze(const Task& source, int minutes, std::time_t now);
    bool Delete(uint32_t id);
    const Task* Find(uint32_t id) const;
    std::vector<Task> List(KindFilter filter) const;
    size_t Clear(KindFilter filter = KindFilter::kAll);
    TickResult Tick(std::time_t now, bool time_valid);

    static std::time_t NextOccurrence(const Task& task, std::time_t after);
    static const char* KindName(Kind kind);
    static const char* RepeatName(Repeat repeat);
    static Kind ParseKind(const std::string& value);
    static KindFilter ParseKindFilter(const std::string& value);
    static Repeat ParseRepeat(const std::string& value);
    static std::string DescribeTask(const Task& task);

private:
    std::vector<Task> tasks_;
    uint32_t next_id_ = 1;
    bool recovery_pending_ = true;
};

class AlertQueue {
public:
    void Enqueue(const Task& task) { pending_.push_back(task); }
    const Task* StartNext();
    std::optional<Task> Stop();
    bool active() const { return active_.has_value(); }
    const Task& current() const;

private:
    std::deque<Task> pending_;
    std::optional<Task> active_;
};

class ReminderDeliverySequence {
public:
    void Begin(bool is_reminder);
    bool OnPlaybackDrained();
    bool OnTtsStarted();
    bool OnTtsStopped();
    bool CancelWaitingForTts();
    bool NeedsServerAbort() const;
    void Reset() { state_ = ReminderDeliveryState::kInactive; }
    ReminderDeliveryState state() const { return state_; }

private:
    ReminderDeliveryState state_ = ReminderDeliveryState::kInactive;
};

}  // namespace schedule

#endif
