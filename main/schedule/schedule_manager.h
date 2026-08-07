#ifndef SCHEDULE_MANAGER_H
#define SCHEDULE_MANAGER_H

#include <cstdint>
#include <ctime>
#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace schedule {

enum class Kind { kAlarm, kReminder };
enum class Repeat { kOnce, kDaily, kWeekdays, kWeekends, kWeekly };

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
    size_t Clear();
    TickResult Tick(std::time_t now, bool time_valid);

    static std::time_t NextOccurrence(const Task& task, std::time_t after);
    static const char* KindName(Kind kind);
    static const char* RepeatName(Repeat repeat);
    static Kind ParseKind(const std::string& value);
    static Repeat ParseRepeat(const std::string& value);

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

}  // namespace schedule

#endif
