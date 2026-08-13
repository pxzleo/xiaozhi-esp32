#ifndef SCHEDULE_MANAGER_H
#define SCHEDULE_MANAGER_H

#include <cstdint>
#include <ctime>
#include <deque>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace schedule {

bool ShouldRestoreTemporaryVolume(uint32_t start_revision, uint32_t current_revision);

enum class Kind { kAlarm, kReminder, kBriefing };
enum class KindFilter { kAll, kAlarm, kReminder, kBriefing };
enum class Repeat { kOnce, kDaily, kWeekdays, kWeekends, kWeekly };
enum class ReminderDeliveryState { kInactive, kWaitingForCue, kWaitingForTts, kSpeaking };
enum class SyncState { kPending, kSynced, kDirty };

struct Task {
    Task() = default;
    Task(uint32_t task_id, Kind task_kind, Repeat task_repeat, std::string task_label,
         std::time_t task_trigger_at, std::vector<int> task_weekdays,
         std::string task_sections, std::string task_location)
        : id(task_id), kind(task_kind), repeat(task_repeat), label(std::move(task_label)),
          trigger_at(task_trigger_at), weekdays(std::move(task_weekdays)),
          sections(std::move(task_sections)), location(std::move(task_location)) {}
    uint32_t id = 0;
    Kind kind = Kind::kReminder;
    Repeat repeat = Repeat::kOnce;
    std::string label;
    std::time_t trigger_at = 0;
    std::vector<int> weekdays;
    std::string sections;
    std::string location;
    std::string source_schedule_id;
    std::string schedule_uuid;
    uint32_t schedule_version = 0;
    SyncState sync_state = SyncState::kPending;
    bool enabled = true;
    bool local_source = true;
};

struct AuthorityTask {
    bool deleted = false;
    uint64_t revision = 0;
    std::string schedule_uuid;
    std::string source_schedule_id;
    Kind kind = Kind::kReminder;
    Repeat repeat = Repeat::kOnce;
    std::string label;
    std::time_t trigger_at = 0;
    std::vector<int> weekdays;
    std::string sections;
    std::string location;
    uint32_t version = 0;
    bool local_source = false;
    bool enabled = true;
    std::time_t last_triggered_at = 0;
    std::time_t snoozed_until = 0;
    bool stop_active = false;
};

struct PendingOccurrence {
    uint32_t local_id = 0;
    std::string source_schedule_id;
    std::string schedule_uuid;
    Kind kind = Kind::kReminder;
    std::string label;
    std::string sections;
    std::string location;
    std::time_t occurrence_at = 0;
};

struct OverflowOccurrence {
    PendingOccurrence occurrence;
    std::time_t first_at = 0;
    uint32_t count = 0;
};

struct SnapshotResult {
    bool changed = false;
    std::vector<uint32_t> removed_local_ids;
};

struct CreateRequest {
    Kind kind = Kind::kReminder;
    Repeat repeat = Repeat::kOnce;
    std::string label;
    std::time_t trigger_at = 0;
    int delay_seconds = 0;
    std::vector<int> weekdays;
    std::string sections;
    std::string location;
};

struct TickResult {
    std::vector<Task> triggered;
    std::vector<Task> missed;
    bool changed = false;
    bool occurrence_outbox_full = false;
};

class Manager {
public:
    static constexpr size_t kMaxTasks = 16;
    // The first 16 entries are the normal retry queue; another 16 persistent
    // slots keep due alarms audible while the server is unavailable.
    static constexpr size_t kMaxPendingOccurrences = 16;
    static constexpr size_t kMaxPersistentFullRecords = 32;
    static constexpr int kOnceRecoveryWindowSeconds = 300;

    const std::vector<Task>& tasks() const { return tasks_; }
    const std::vector<PendingOccurrence>& pending_occurrences() const {
        return pending_occurrences_;
    }
    const std::vector<OverflowOccurrence>& overflow_occurrences() const {
        return overflow_occurrences_;
    }
    uint32_t next_id() const { return next_id_; }
    uint64_t snapshot_revision() const { return snapshot_revision_; }
    uint64_t action_revision() const { return action_revision_; }
    bool full_snapshot_in_progress() const { return full_snapshot_in_progress_; }
    const std::vector<std::string>& full_snapshot_seen_uuids() const {
        return full_snapshot_seen_uuids_;
    }
    const std::vector<AuthorityTask>& full_snapshot_staged_tasks() const {
        return full_snapshot_staged_tasks_;
    }
    void Restore(std::vector<Task> tasks, uint32_t next_id,
                 std::vector<PendingOccurrence> pending_occurrences = {},
                 uint64_t snapshot_revision = 0, uint64_t action_revision = 0,
                 bool full_snapshot_in_progress = false,
                 std::vector<std::string> full_snapshot_seen_uuids = {},
                 std::vector<AuthorityTask> full_snapshot_staged_tasks = {},
                 std::vector<OverflowOccurrence> overflow_occurrences = {});
    Task Create(const CreateRequest& request, std::time_t now);
    Task Snooze(const Task& source, int minutes, std::time_t now);
    bool Delete(uint32_t id);
    bool DeleteByScheduleUuid(const std::string& schedule_uuid);
    const Task* Find(uint32_t id) const;
    const Task* FindByScheduleUuid(const std::string& schedule_uuid) const;
    std::vector<Task> List(KindFilter filter, bool enabled_only = true) const;
    size_t Clear(KindFilter filter = KindFilter::kAll);
    TickResult Tick(std::time_t now, bool time_valid);
    bool BindAuthority(uint32_t local_id, const std::string& schedule_uuid,
                       uint32_t version);
    bool RecordOccurrence(const Task& task, std::time_t occurrence_at);
    bool PromoteOverflowOccurrence();
    bool AcknowledgeOccurrence(const std::string& schedule_uuid, uint32_t local_id,
                               std::time_t occurrence_at);
    SnapshotResult ApplyAuthorityChanges(uint64_t cursor,
                                         const std::vector<AuthorityTask>& changes,
                                         bool full_snapshot = false,
                                         bool has_more = false);
    bool ApplyAuthorityAction(uint64_t revision, const std::string& schedule_uuid,
                              uint32_t schedule_version, const std::string& action,
                              std::time_t snoozed_until, std::time_t next_trigger_at,
                              const Task* active_source = nullptr);
    size_t RemovePendingOccurrences(const std::string& schedule_uuid,
                                    const std::string& source_schedule_id,
                                    bool local_source,
                                    std::time_t through_occurrence = 0);

    static std::time_t NextOccurrence(const Task& task, std::time_t after);
    static const char* KindName(Kind kind);
    static const char* RepeatName(Repeat repeat);
    static Kind ParseKind(const std::string& value);
    static KindFilter ParseKindFilter(const std::string& value);
    static Repeat ParseRepeat(const std::string& value);
    static std::string DescribeTask(const Task& task);
    static std::string DescribeCreation(const Task& task, std::time_t now);
    static bool ShouldSuggestRepeating(const std::vector<Task>& tasks,
                                       const Task& created);

private:
    std::vector<Task> tasks_;
    std::vector<PendingOccurrence> pending_occurrences_;
    std::vector<OverflowOccurrence> overflow_occurrences_;
    uint32_t next_id_ = 1;
    uint64_t snapshot_revision_ = 0;
    uint64_t action_revision_ = 0;
    bool full_snapshot_in_progress_ = false;
    std::vector<std::string> full_snapshot_seen_uuids_;
    std::vector<AuthorityTask> full_snapshot_staged_tasks_;
    bool recovery_pending_ = true;
};

class AlertQueue {
public:
    void Enqueue(const Task& task) { pending_.push_back(task); }
    const Task* StartNext();
    std::optional<Task> Stop();
    size_t Remove(const std::string& schedule_uuid,
                  const std::string& source_schedule_id, bool local_source);
    bool active() const { return active_.has_value(); }
    const Task& current() const;

private:
    std::deque<Task> pending_;
    std::optional<Task> active_;
};

class ReminderDeliverySequence {
public:
    void Begin();
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
