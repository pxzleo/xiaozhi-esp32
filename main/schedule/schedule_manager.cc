#include "schedule_manager.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <unordered_set>

namespace schedule {

bool ShouldRestoreTemporaryVolume(uint32_t start_revision, uint32_t current_revision) {
    return start_revision == current_revision;
}

static bool SameLocalClockOnDifferentDate(std::time_t left, std::time_t right) {
    std::tm left_time{};
    std::tm right_time{};
    localtime_r(&left, &left_time);
    localtime_r(&right, &right_time);
    const bool different_date = left_time.tm_year != right_time.tm_year ||
                                left_time.tm_yday != right_time.tm_yday;
    return different_date && left_time.tm_hour == right_time.tm_hour &&
           left_time.tm_min == right_time.tm_min &&
           left_time.tm_sec == right_time.tm_sec;
}

namespace {

struct Utf8Stats {
    size_t count = 0;
    bool valid = true;
    bool has_non_whitespace = false;
};

bool IsUnicodeWhitespace(uint32_t codepoint) {
    return (codepoint >= 0x0009 && codepoint <= 0x000d) || codepoint == 0x0020 ||
        codepoint == 0x0085 || codepoint == 0x00a0 || codepoint == 0x1680 ||
        (codepoint >= 0x2000 && codepoint <= 0x200a) || codepoint == 0x2028 ||
        codepoint == 0x2029 || codepoint == 0x202f || codepoint == 0x205f ||
        codepoint == 0x3000;
}

Utf8Stats AnalyzeUtf8(const std::string& value) {
    Utf8Stats stats;
    for (size_t i = 0; i < value.size();) {
        const auto first = static_cast<unsigned char>(value[i]);
        size_t length = 0;
        if (first <= 0x7f) {
            length = 1;
        } else if (first >= 0xc2 && first <= 0xdf) {
            length = 2;
        } else if (first >= 0xe0 && first <= 0xef) {
            length = 3;
        } else if (first >= 0xf0 && first <= 0xf4) {
            length = 4;
        } else {
            stats.valid = false;
            return stats;
        }
        if (i + length > value.size()) {
            stats.valid = false;
            return stats;
        }
        for (size_t j = 1; j < length; ++j) {
            if ((static_cast<unsigned char>(value[i + j]) & 0xc0) != 0x80) {
                stats.valid = false;
                return stats;
            }
        }
        if ((first == 0xe0 && static_cast<unsigned char>(value[i + 1]) < 0xa0) ||
            (first == 0xed && static_cast<unsigned char>(value[i + 1]) >= 0xa0) ||
            (first == 0xf0 && static_cast<unsigned char>(value[i + 1]) < 0x90) ||
            (first == 0xf4 && static_cast<unsigned char>(value[i + 1]) >= 0x90)) {
            stats.valid = false;
            return stats;
        }
        uint32_t codepoint = first;
        if (length > 1) codepoint &= (1u << (7 - length)) - 1;
        for (size_t j = 1; j < length; ++j) {
            codepoint = (codepoint << 6) | (static_cast<unsigned char>(value[i + j]) & 0x3f);
        }
        stats.has_non_whitespace = stats.has_non_whitespace || !IsUnicodeWhitespace(codepoint);
        i += length;
        ++stats.count;
    }
    return stats;
}

bool ContainsDay(const std::vector<int>& days, int day) {
    return std::find(days.begin(), days.end(), day) != days.end();
}

int IsoWeekday(const std::tm& local) { return local.tm_wday == 0 ? 7 : local.tm_wday; }

bool MatchesFilter(const Task& task, KindFilter filter) {
    return filter == KindFilter::kAll ||
        (filter == KindFilter::kAlarm && task.kind == Kind::kAlarm) ||
        (filter == KindFilter::kReminder && task.kind == Kind::kReminder) ||
        (filter == KindFilter::kBriefing && task.kind == Kind::kBriefing);
}

void ValidateBriefing(const Kind kind, const std::string& sections,
                      const std::string& location) {
    if (kind != Kind::kBriefing) {
        if (!sections.empty() || !location.empty()) {
            throw std::invalid_argument("只有briefing任务可以提供sections和location");
        }
        return;
    }
    if (sections != "weather" && sections != "news" && sections != "weather,news") {
        throw std::invalid_argument("sections必须是weather/news/weather,news之一");
    }
    const auto place = AnalyzeUtf8(location);
    if (!place.valid || place.count > 40 ||
        (sections.find("weather") != std::string::npos &&
         (place.count < 1 || !place.has_non_whitespace))) {
        throw std::invalid_argument("天气简报地点必须是1到40个Unicode字符");
    }
}

const char* WeekdayChinese(int weekday) {
    static constexpr const char* kWeekdays[] = {
        "日", "一", "二", "三", "四", "五", "六",
    };
    return weekday >= 0 && weekday <= 6 ? kWeekdays[weekday] : "";
}

std::string FormatClock(std::time_t timestamp) {
    std::tm local{};
    localtime_r(&timestamp, &local);
    std::string result = std::to_string(local.tm_hour) + "点";
    if (local.tm_min != 0) result += std::to_string(local.tm_min) + "分";
    return result;
}

bool IsUuidV4(const std::string& value) {
    if (value.size() != 36 || value[8] != '-' || value[13] != '-' || value[18] != '-' ||
        value[23] != '-' || value[14] != '4' ||
        std::string("89abAB").find(value[19]) == std::string::npos) {
        return false;
    }
    for (size_t i = 0; i < value.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) continue;
        if (!std::isxdigit(static_cast<unsigned char>(value[i]))) return false;
    }
    return true;
}

bool IsSourceScheduleId(const std::string& value) {
    return !value.empty() && value.size() <= 64 &&
        std::all_of(value.begin(), value.end(), [](unsigned char character) {
            return std::isdigit(character) != 0;
        });
}

void ValidateTaskFields(const Task& task) {
    const auto label = AnalyzeUtf8(task.label);
    if (task.id == 0 || task.trigger_at <= 0 || !label.valid || label.count < 1 ||
        label.count > 80 || !label.has_non_whitespace) {
        throw std::invalid_argument("保存的定时任务字段无效");
    }
    if (task.repeat == Repeat::kWeekly && task.weekdays.empty()) {
        throw std::invalid_argument("保存的weekly任务缺少weekdays");
    }
    ValidateBriefing(task.kind, task.sections, task.location);
    for (int day : task.weekdays) {
        if (day < 1 || day > 7) throw std::invalid_argument("保存的weekday超出1到7");
    }
    if (!task.source_schedule_id.empty() && !IsSourceScheduleId(task.source_schedule_id)) {
        throw std::invalid_argument("保存的源日程ID无效");
    }
    if (!task.schedule_uuid.empty() && !IsUuidV4(task.schedule_uuid)) {
        throw std::invalid_argument("保存的共享日程UUID无效");
    }
    if (task.sync_state == SyncState::kSynced &&
        (task.schedule_uuid.empty() || task.schedule_version == 0)) {
        throw std::invalid_argument("已同步日程缺少权威版本");
    }
}

}  // namespace

void Manager::Restore(std::vector<Task> tasks, uint32_t next_id,
                      std::vector<PendingOccurrence> pending_occurrences,
                      uint64_t snapshot_revision, uint64_t action_revision,
                      bool full_snapshot_in_progress,
                      std::vector<std::string> full_snapshot_seen_uuids,
                      std::vector<AuthorityTask> full_snapshot_staged_tasks,
                      std::vector<OverflowOccurrence> overflow_occurrences) {
    if (tasks.size() > kMaxTasks) {
        throw std::invalid_argument("保存的定时任务超过16项");
    }
    uint32_t max_id = 0;
    for (auto& task : tasks) {
        if (task.source_schedule_id.empty()) task.source_schedule_id = std::to_string(task.id);
        ValidateTaskFields(task);
        max_id = std::max(max_id, task.id);
    }
    if (next_id <= max_id) throw std::invalid_argument("保存的next_id没有递增");
    if (pending_occurrences.size() > kMaxPendingOccurrences) {
        throw std::invalid_argument("保存的离线触发记录超过16项");
    }
    for (const auto& occurrence : pending_occurrences) {
        if (occurrence.local_id == 0 || occurrence.occurrence_at <= 0 ||
            !IsSourceScheduleId(occurrence.source_schedule_id) ||
            (!occurrence.schedule_uuid.empty() && !IsUuidV4(occurrence.schedule_uuid))) {
            throw std::invalid_argument("保存的离线触发记录无效");
        }
    }
    if (overflow_occurrences.size() > kMaxTasks) {
        throw std::invalid_argument("保存的离线触发溢出记录超过16项");
    }
    for (const auto& overflow : overflow_occurrences) {
        const auto& occurrence = overflow.occurrence;
        if (overflow.count == 0 || overflow.first_at <= 0 ||
            occurrence.local_id == 0 || occurrence.occurrence_at < overflow.first_at ||
            !IsSourceScheduleId(occurrence.source_schedule_id) ||
            (!occurrence.schedule_uuid.empty() && !IsUuidV4(occurrence.schedule_uuid))) {
            throw std::invalid_argument("保存的离线触发溢出记录无效");
        }
    }
    if (!full_snapshot_in_progress && !full_snapshot_seen_uuids.empty()) {
        throw std::invalid_argument("未进行完整快照时不能保存已见UUID");
    }
    if (!full_snapshot_in_progress && !full_snapshot_staged_tasks.empty()) {
        throw std::invalid_argument("未进行完整快照时不能保存暂存任务");
    }
    if (full_snapshot_seen_uuids.size() > kMaxTasks) {
        throw std::invalid_argument("完整快照已见UUID超过本地容量");
    }
    std::unordered_set<std::string> seen;
    for (const auto& uuid : full_snapshot_seen_uuids) {
        if (!IsUuidV4(uuid) || !seen.insert(uuid).second) {
            throw std::invalid_argument("完整快照已见UUID无效");
        }
    }
    if (full_snapshot_staged_tasks.size() > kMaxTasks) {
        throw std::invalid_argument("完整快照暂存任务超过本地容量");
    }
    std::unordered_set<std::string> staged_uuids;
    for (const auto& authority : full_snapshot_staged_tasks) {
        if (authority.deleted || authority.revision == 0 ||
            !IsUuidV4(authority.schedule_uuid) || authority.version == 0 ||
            !IsSourceScheduleId(authority.source_schedule_id) ||
            !staged_uuids.insert(authority.schedule_uuid).second) {
            throw std::invalid_argument("完整快照暂存任务无效");
        }
        Task staged;
        staged.id = 1;
        staged.kind = authority.kind;
        staged.repeat = authority.repeat;
        staged.label = authority.label;
        staged.trigger_at = authority.trigger_at;
        staged.weekdays = authority.weekdays;
        staged.sections = authority.sections;
        staged.location = authority.location;
        staged.source_schedule_id = authority.source_schedule_id;
        staged.schedule_uuid = authority.schedule_uuid;
        staged.schedule_version = authority.version;
        staged.sync_state = SyncState::kSynced;
        ValidateTaskFields(staged);
    }
    tasks_ = std::move(tasks);
    pending_occurrences_ = std::move(pending_occurrences);
    overflow_occurrences_ = std::move(overflow_occurrences);
    next_id_ = std::max<uint32_t>(next_id, 1);
    snapshot_revision_ = snapshot_revision;
    action_revision_ = action_revision;
    full_snapshot_in_progress_ = full_snapshot_in_progress;
    full_snapshot_seen_uuids_ = std::move(full_snapshot_seen_uuids);
    full_snapshot_staged_tasks_ = std::move(full_snapshot_staged_tasks);
    recovery_pending_ = true;
}

Task Manager::Create(const CreateRequest& request, std::time_t now) {
    if (tasks_.size() >= kMaxTasks) {
        throw std::runtime_error("定时任务已达16项上限");
    }
    const auto label = AnalyzeUtf8(request.label);
    if (!label.valid || label.count < 1 || label.count > 80 || !label.has_non_whitespace) {
        throw std::invalid_argument("内容必须是1到80个Unicode字符且不能全为空白");
    }
    ValidateBriefing(request.kind, request.sections, request.location);
    const bool has_absolute = request.trigger_at > 0;
    const bool has_delay = request.delay_seconds > 0;
    if (has_absolute == has_delay) {
        throw std::invalid_argument("trigger_at和delay_seconds必须且只能提供一个");
    }
    if (has_delay && request.repeat != Repeat::kOnce) {
        throw std::invalid_argument("delay_seconds只支持once任务");
    }
    if (request.repeat == Repeat::kWeekly) {
        if (request.weekdays.empty()) {
            throw std::invalid_argument("weekly任务必须提供weekdays");
        }
        for (int day : request.weekdays) {
            if (day < 1 || day > 7) {
                throw std::invalid_argument("weekdays必须在1到7之间");
            }
        }
    } else if (!request.weekdays.empty()) {
        throw std::invalid_argument("只有weekly任务可以提供weekdays");
    }

    const std::time_t first_trigger = has_delay ? now + request.delay_seconds : request.trigger_at;
    std::tm first_local{};
    if (localtime_r(&first_trigger, &first_local) == nullptr) {
        throw std::invalid_argument("trigger_at不是有效本地时间");
    }
    const int first_weekday = IsoWeekday(first_local);
    if ((request.repeat == Repeat::kWeekdays && first_weekday > 5) ||
        (request.repeat == Repeat::kWeekends && first_weekday < 6) ||
        (request.repeat == Repeat::kWeekly &&
         !ContainsDay(request.weekdays, first_weekday))) {
        throw std::invalid_argument("首次trigger_at不符合repeat重复规则");
    }

    Task task;
    task.id = next_id_++;
    task.kind = request.kind;
    task.repeat = request.repeat;
    task.label = request.label;
    task.sections = request.sections;
    task.location = request.location;
    task.trigger_at = first_trigger;
    task.weekdays = request.weekdays;
    std::sort(task.weekdays.begin(), task.weekdays.end());
    task.weekdays.erase(std::unique(task.weekdays.begin(), task.weekdays.end()),
                        task.weekdays.end());
    task.source_schedule_id = std::to_string(task.id);
    task.local_source = true;
    tasks_.push_back(task);
    return task;
}

Task Manager::Snooze(const Task& source, int minutes, std::time_t now) {
    if (source.kind == Kind::kBriefing) {
        throw std::invalid_argument("每日简报不支持稍后提醒");
    }
    if (minutes < 1 || minutes > 60) {
        throw std::invalid_argument("稍后提醒分钟数必须在1到60之间");
    }
    CreateRequest request;
    request.kind = source.kind;
    request.repeat = Repeat::kOnce;
    request.label = source.label;
    request.delay_seconds = minutes * 60;
    const auto created = Create(request, now);
    auto found = std::find_if(tasks_.begin(), tasks_.end(),
                              [id = created.id](const Task& task) { return task.id == id; });
    found->source_schedule_id = source.source_schedule_id;
    found->schedule_uuid = source.schedule_uuid;
    found->schedule_version = source.schedule_version;
    found->sync_state = source.schedule_uuid.empty() ? SyncState::kPending : SyncState::kDirty;
    return *found;
}

bool Manager::Delete(uint32_t id) {
    const auto old_size = tasks_.size();
    tasks_.erase(std::remove_if(tasks_.begin(), tasks_.end(),
                                [id](const Task& task) { return task.id == id; }),
                 tasks_.end());
    return tasks_.size() != old_size;
}

bool Manager::DeleteByScheduleUuid(const std::string& schedule_uuid) {
    const auto old_size = tasks_.size();
    tasks_.erase(std::remove_if(tasks_.begin(), tasks_.end(),
        [&schedule_uuid](const Task& task) { return task.schedule_uuid == schedule_uuid; }),
        tasks_.end());
    return tasks_.size() != old_size;
}

const Task* Manager::Find(uint32_t id) const {
    auto found = std::find_if(tasks_.begin(), tasks_.end(),
                              [id](const Task& task) { return task.id == id; });
    return found == tasks_.end() ? nullptr : &*found;
}

const Task* Manager::FindByScheduleUuid(const std::string& schedule_uuid) const {
    auto found = std::find_if(tasks_.begin(), tasks_.end(),
                              [&schedule_uuid](const Task& task) {
                                  return task.schedule_uuid == schedule_uuid;
                              });
    return found == tasks_.end() ? nullptr : &*found;
}

bool Manager::BindAuthority(uint32_t local_id, const std::string& schedule_uuid,
                            uint32_t version) {
    if (!IsUuidV4(schedule_uuid) || version == 0) {
        throw std::invalid_argument("共享日程权威标识无效");
    }
    auto found = std::find_if(tasks_.begin(), tasks_.end(),
                              [local_id](const Task& task) { return task.id == local_id; });
    if (found == tasks_.end()) return false;
    const auto duplicate = std::find_if(tasks_.begin(), tasks_.end(),
        [&schedule_uuid, local_id](const Task& task) {
            return task.id != local_id && task.schedule_uuid == schedule_uuid;
        });
    if (duplicate != tasks_.end()) throw std::invalid_argument("共享日程UUID重复");
    found->schedule_uuid = schedule_uuid;
    found->schedule_version = version;
    found->sync_state = SyncState::kSynced;
    return true;
}

bool Manager::RecordOccurrence(const Task& task, std::time_t occurrence_at) {
    if (occurrence_at <= 0) throw std::invalid_argument("触发时间无效");
    const auto duplicate = std::find_if(pending_occurrences_.begin(),
        pending_occurrences_.end(), [&task, occurrence_at](const PendingOccurrence& item) {
            return item.local_id == task.id && item.occurrence_at == occurrence_at;
        });
    if (duplicate != pending_occurrences_.end()) return true;
    if (pending_occurrences_.size() >= kMaxPendingOccurrences) {
        auto overflow = std::find_if(overflow_occurrences_.begin(),
            overflow_occurrences_.end(), [&](const OverflowOccurrence& item) {
                return !task.schedule_uuid.empty()
                    ? item.occurrence.schedule_uuid == task.schedule_uuid
                    : item.occurrence.local_id == task.id;
            });
        if (overflow == overflow_occurrences_.end()) {
            if (overflow_occurrences_.size() >= kMaxTasks) return false;
            PendingOccurrence occurrence{task.id, task.source_schedule_id,
                task.schedule_uuid, task.kind, task.label, task.sections, task.location,
                occurrence_at};
            overflow_occurrences_.push_back({std::move(occurrence), occurrence_at, 1});
        } else {
            overflow->occurrence.occurrence_at = occurrence_at;
            ++overflow->count;
        }
        return true;
    }
    pending_occurrences_.push_back({task.id, task.source_schedule_id, task.schedule_uuid,
                                    task.kind, task.label, task.sections, task.location,
                                    occurrence_at});
    return true;
}

bool Manager::PromoteOverflowOccurrence() {
    if (overflow_occurrences_.empty() ||
        pending_occurrences_.size() >= kMaxPendingOccurrences) return false;
    pending_occurrences_.push_back(overflow_occurrences_.front().occurrence);
    overflow_occurrences_.erase(overflow_occurrences_.begin());
    return true;
}

bool Manager::AcknowledgeOccurrence(const std::string& schedule_uuid, uint32_t local_id,
                                    std::time_t occurrence_at) {
    const auto old_size = pending_occurrences_.size();
    pending_occurrences_.erase(std::remove_if(pending_occurrences_.begin(),
        pending_occurrences_.end(), [&](const PendingOccurrence& item) {
            const bool same_schedule = !schedule_uuid.empty()
                ? item.schedule_uuid == schedule_uuid : item.local_id == local_id;
            return same_schedule && item.occurrence_at <= occurrence_at;
        }), pending_occurrences_.end());
    const bool removed = pending_occurrences_.size() != old_size;
    PromoteOverflowOccurrence();
    return removed;
}

size_t Manager::RemovePendingOccurrences(const std::string& schedule_uuid,
                                         const std::string& source_schedule_id,
                                         bool local_source,
                                         std::time_t through_occurrence) {
    const auto old_size = pending_occurrences_.size();
    pending_occurrences_.erase(std::remove_if(pending_occurrences_.begin(),
        pending_occurrences_.end(), [&](const PendingOccurrence& occurrence) {
            const bool matches = (!schedule_uuid.empty() &&
                                  occurrence.schedule_uuid == schedule_uuid) ||
                (local_source && occurrence.schedule_uuid.empty() &&
                 occurrence.source_schedule_id == source_schedule_id);
            return matches && (through_occurrence <= 0 ||
                               occurrence.occurrence_at <= through_occurrence);
        }), pending_occurrences_.end());
    overflow_occurrences_.erase(std::remove_if(overflow_occurrences_.begin(),
        overflow_occurrences_.end(), [&](const OverflowOccurrence& item) {
            const auto& occurrence = item.occurrence;
            const bool matches = (!schedule_uuid.empty() &&
                                  occurrence.schedule_uuid == schedule_uuid) ||
                (local_source && occurrence.schedule_uuid.empty() &&
                 occurrence.source_schedule_id == source_schedule_id);
            return matches && (through_occurrence <= 0 ||
                               occurrence.occurrence_at <= through_occurrence);
        }), overflow_occurrences_.end());
    while (PromoteOverflowOccurrence()) {}
    return old_size - pending_occurrences_.size();
}

SnapshotResult Manager::ApplyAuthorityChanges(
    uint64_t cursor, const std::vector<AuthorityTask>& authority_tasks,
    bool full_snapshot, bool has_more) {
    if (cursor == 0) {
        if (!authority_tasks.empty()) {
            throw std::invalid_argument("零游标不能携带共享日程变更");
        }
    }
    const bool starting_full_snapshot = full_snapshot;
    const bool collecting_full_snapshot = starting_full_snapshot || full_snapshot_in_progress_;
    const uint64_t validation_revision = starting_full_snapshot ? 0 : snapshot_revision_;
    if (!collecting_full_snapshot && cursor <= snapshot_revision_) return {};
    if (collecting_full_snapshot && !starting_full_snapshot && cursor < snapshot_revision_) {
        throw std::invalid_argument("完整快照分页游标倒退");
    }
    std::unordered_set<std::string> uuids;
    for (const auto& authority : authority_tasks) {
        if (authority.revision == 0 || authority.revision > cursor ||
            authority.revision <= validation_revision ||
            !IsUuidV4(authority.schedule_uuid) ||
            authority.version == 0 || !IsSourceScheduleId(authority.source_schedule_id)) {
            throw std::invalid_argument("共享日程变更版本无效");
        }
        if (authority.deleted) continue;
        Task validated;
        validated.id = 1;
        validated.kind = authority.kind;
        validated.repeat = authority.repeat;
        validated.label = authority.label;
        validated.trigger_at = authority.trigger_at;
        validated.weekdays = authority.weekdays;
        validated.sections = authority.sections;
        validated.location = authority.location;
        validated.source_schedule_id = authority.source_schedule_id;
        validated.schedule_uuid = authority.schedule_uuid;
        validated.schedule_version = authority.version;
        validated.sync_state = SyncState::kSynced;
        ValidateTaskFields(validated);
        if (!uuids.insert(authority.schedule_uuid).second) {
            throw std::invalid_argument("共享日程快照UUID重复");
        }
    }

    std::vector<AuthorityTask> staged = starting_full_snapshot
        ? std::vector<AuthorityTask>{} : full_snapshot_staged_tasks_;
    if (collecting_full_snapshot) {
        for (const auto& authority : authority_tasks) {
            auto found = std::find_if(staged.begin(), staged.end(), [&](const AuthorityTask& item) {
                return item.schedule_uuid == authority.schedule_uuid;
            });
            if (authority.deleted) {
                if (found != staged.end()) staged.erase(found);
                continue;
            }
            if (found == staged.end()) {
                if (staged.size() >= kMaxTasks) {
                    throw std::runtime_error("完整快照权威任务超过16项容量");
                }
                staged.push_back(authority);
            } else {
                *found = authority;
            }
        }
        if (tasks_.size() + staged.size() + pending_occurrences_.size() >
            kMaxPersistentFullRecords) {
            throw std::runtime_error("完整快照与离线记录超过NVS持久预算");
        }
        if (has_more) {
            full_snapshot_in_progress_ = true;
            full_snapshot_staged_tasks_ = std::move(staged);
            full_snapshot_seen_uuids_.clear();
            for (const auto& authority : full_snapshot_staged_tasks_) {
                full_snapshot_seen_uuids_.push_back(authority.schedule_uuid);
            }
            snapshot_revision_ = cursor;
            return {true, {}};
        }
    }

    const auto& applied_authority_tasks = collecting_full_snapshot ? staged : authority_tasks;

    auto updated = tasks_;
    auto updated_occurrences = pending_occurrences_;
    auto updated_overflow = overflow_occurrences_;
    uint32_t updated_next_id = next_id_;
    SnapshotResult result;
    if (collecting_full_snapshot) {
        const auto present = [&](const std::string& uuid) {
            return std::any_of(staged.begin(), staged.end(), [&](const AuthorityTask& item) {
                return item.schedule_uuid == uuid;
            });
        };
        updated.erase(std::remove_if(updated.begin(), updated.end(), [&](const Task& task) {
            if (task.schedule_uuid.empty() || present(task.schedule_uuid)) return false;
            result.removed_local_ids.push_back(task.id);
            return true;
        }), updated.end());
    }
    for (const auto& authority : applied_authority_tasks) {
        if (authority.deleted) continue;
        auto found = std::find_if(updated.begin(), updated.end(),
            [&authority](const Task& task) {
                if (!task.schedule_uuid.empty()) {
                    return task.schedule_uuid == authority.schedule_uuid;
                }
                return authority.local_source && task.sync_state == SyncState::kPending &&
                    task.source_schedule_id == authority.source_schedule_id;
            });
        if (authority.local_source) {
            for (auto& occurrence : updated_occurrences) {
                if (occurrence.source_schedule_id == authority.source_schedule_id) {
                    occurrence.schedule_uuid = authority.schedule_uuid;
                }
            }
            for (auto& overflow : updated_overflow) {
                if (overflow.occurrence.source_schedule_id == authority.source_schedule_id) {
                    overflow.occurrence.schedule_uuid = authority.schedule_uuid;
                }
            }
        }
        if (authority.last_triggered_at > 0) {
            updated_occurrences.erase(std::remove_if(updated_occurrences.begin(),
                updated_occurrences.end(), [&authority](const PendingOccurrence& occurrence) {
                    return occurrence.schedule_uuid == authority.schedule_uuid &&
                        occurrence.occurrence_at <= authority.last_triggered_at;
                }), updated_occurrences.end());
            updated_overflow.erase(std::remove_if(updated_overflow.begin(),
                updated_overflow.end(), [&authority](const OverflowOccurrence& overflow) {
                    return overflow.occurrence.schedule_uuid == authority.schedule_uuid &&
                        overflow.occurrence.occurrence_at <= authority.last_triggered_at;
                }), updated_overflow.end());
        }
        if (found != updated.end() && !found->schedule_uuid.empty() &&
            (found->schedule_version > authority.version ||
             (found->sync_state == SyncState::kDirty &&
              found->schedule_version == authority.version))) {
            continue;
        }
        if (found != updated.end() && found->schedule_uuid == authority.schedule_uuid &&
            found->schedule_version == authority.version &&
            found->sync_state == SyncState::kSynced) {
            continue;
        }
        if (found == updated.end() && !authority.enabled) {
            continue;
        }
        if (found == updated.end()) {
            if (updated.size() >= kMaxTasks) {
                throw std::runtime_error("共享日程同步后超过16项容量");
            }
            Task task;
            task.id = updated_next_id++;
            updated.push_back(std::move(task));
            found = std::prev(updated.end());
        }
        found->kind = authority.kind;
        found->repeat = authority.repeat;
        found->label = authority.label;
        found->trigger_at = authority.snoozed_until > 0 && authority.repeat == Repeat::kOnce
            ? authority.snoozed_until : authority.trigger_at;
        found->weekdays = authority.weekdays;
        found->sections = authority.sections;
        found->location = authority.location;
        found->source_schedule_id = authority.source_schedule_id;
        found->schedule_uuid = authority.schedule_uuid;
        found->schedule_version = authority.version;
        found->sync_state = SyncState::kSynced;
        found->enabled = authority.enabled;
        found->local_source = authority.local_source;
        if (authority.snoozed_until > 0 && authority.repeat != Repeat::kOnce) {
            const auto override = std::find_if(updated.begin(), updated.end(),
                [&](const Task& task) {
                    return task.schedule_uuid == authority.schedule_uuid &&
                        task.repeat == Repeat::kOnce &&
                        task.trigger_at == authority.snoozed_until;
                });
            if (override == updated.end()) {
                if (updated.size() >= kMaxTasks) {
                    throw std::runtime_error("共享日程同步后超过16项容量");
                }
                Task copy = *found;
                copy.id = updated_next_id++;
                copy.repeat = Repeat::kOnce;
                copy.weekdays.clear();
                copy.trigger_at = authority.snoozed_until;
                copy.local_source = authority.local_source;
                updated.push_back(std::move(copy));
            } else {
                override->schedule_version = authority.version;
                override->sync_state = SyncState::kSynced;
                override->enabled = true;
            }
        } else if (authority.repeat != Repeat::kOnce) {
            const uint32_t canonical_id = found->id;
            updated.erase(std::remove_if(updated.begin(), updated.end(),
                [&](const Task& task) {
                    return task.id != canonical_id &&
                        task.schedule_uuid == authority.schedule_uuid &&
                        task.repeat == Repeat::kOnce;
                }), updated.end());
        }
    }

    for (const auto& authority : applied_authority_tasks) {
        if (!authority.deleted) continue;
        updated.erase(std::remove_if(updated.begin(), updated.end(), [&](const Task& task) {
            const bool matches = task.schedule_uuid == authority.schedule_uuid ||
                (authority.local_source && task.schedule_uuid.empty() &&
                 task.source_schedule_id == authority.source_schedule_id);
            if (!matches) return false;
            result.removed_local_ids.push_back(task.id);
            return true;
        }), updated.end());
        updated_occurrences.erase(std::remove_if(updated_occurrences.begin(),
            updated_occurrences.end(), [&](const PendingOccurrence& occurrence) {
                return occurrence.schedule_uuid == authority.schedule_uuid ||
                    (authority.local_source && occurrence.schedule_uuid.empty() &&
                     occurrence.source_schedule_id == authority.source_schedule_id);
        }), updated_occurrences.end());
        updated_overflow.erase(std::remove_if(updated_overflow.begin(),
            updated_overflow.end(), [&](const OverflowOccurrence& overflow) {
                const auto& occurrence = overflow.occurrence;
                return occurrence.schedule_uuid == authority.schedule_uuid ||
                    (authority.local_source && occurrence.schedule_uuid.empty() &&
                     occurrence.source_schedule_id == authority.source_schedule_id);
            }), updated_overflow.end());
    }
    bool updated_snapshot_in_progress = false;
    if (collecting_full_snapshot && !has_more) {
        const auto was_seen = [&](const std::string& uuid) {
            return std::any_of(staged.begin(), staged.end(), [&](const AuthorityTask& item) {
                return item.schedule_uuid == uuid;
            });
        };
        updated.erase(std::remove_if(updated.begin(), updated.end(), [&](const Task& task) {
            if (task.schedule_uuid.empty() || was_seen(task.schedule_uuid)) return false;
            result.removed_local_ids.push_back(task.id);
            return true;
        }), updated.end());
        updated_occurrences.erase(std::remove_if(updated_occurrences.begin(),
            updated_occurrences.end(), [&](const PendingOccurrence& occurrence) {
                return !occurrence.schedule_uuid.empty() &&
                    !was_seen(occurrence.schedule_uuid);
            }), updated_occurrences.end());
        updated_overflow.erase(std::remove_if(updated_overflow.begin(),
            updated_overflow.end(), [&](const OverflowOccurrence& overflow) {
                return !overflow.occurrence.schedule_uuid.empty() &&
                    !was_seen(overflow.occurrence.schedule_uuid);
            }), updated_overflow.end());
    }
    while (!updated_overflow.empty() && updated_occurrences.size() < kMaxPendingOccurrences) {
        updated_occurrences.push_back(updated_overflow.front().occurrence);
        updated_overflow.erase(updated_overflow.begin());
    }
    result.changed = true;
    tasks_ = std::move(updated);
    pending_occurrences_ = std::move(updated_occurrences);
    overflow_occurrences_ = std::move(updated_overflow);
    next_id_ = updated_next_id;
    snapshot_revision_ = cursor;
    full_snapshot_in_progress_ = updated_snapshot_in_progress;
    full_snapshot_seen_uuids_.clear();
    full_snapshot_staged_tasks_.clear();
    return result;
}

bool Manager::ApplyAuthorityAction(uint64_t revision, const std::string& schedule_uuid,
                                   uint32_t schedule_version, const std::string& action,
                                   std::time_t snoozed_until, std::time_t next_trigger_at,
                                   const Task* active_source) {
    if (revision == 0 || !IsUuidV4(schedule_uuid) || schedule_version == 0 ||
        (action != "stop" && action != "complete" && action != "snooze" &&
         action != "delete")) {
        throw std::invalid_argument("共享日程动作字段无效");
    }
    if (revision <= action_revision_) return false;
    auto updated = tasks_;
    uint32_t updated_next_id = next_id_;
    const auto is_target = [&schedule_uuid](const Task& task) {
        return task.schedule_uuid == schedule_uuid;
    };
    if (std::any_of(updated.begin(), updated.end(), [&](const Task& task) {
            return is_target(task) && task.schedule_version > schedule_version;
        })) {
        throw std::runtime_error("共享日程动作版本早于本地副本");
    }
    auto found = std::find_if(updated.begin(), updated.end(), is_target);
    if (action == "delete" || ((action == "stop" || action == "complete") &&
                                next_trigger_at <= 0)) {
        updated.erase(std::remove_if(updated.begin(), updated.end(), is_target), updated.end());
        RemovePendingOccurrences(schedule_uuid, "", false);
    } else if (action == "snooze") {
        if (snoozed_until <= 0) throw std::invalid_argument("稍后提醒缺少统一时间");
        if (found == updated.end() && active_source != nullptr) {
            if (updated.size() >= kMaxTasks) {
                throw std::runtime_error("稍后提醒无法写入已满的本地日程");
            }
            Task copy = *active_source;
            copy.id = updated_next_id++;
            copy.schedule_uuid = schedule_uuid;
            updated.push_back(std::move(copy));
            found = std::prev(updated.end());
        }
        if (found == updated.end()) {
            action_revision_ = revision;
            return true;
        }
        const bool recurring = std::any_of(updated.begin(), updated.end(),
            [&](const Task& task) { return is_target(task) && task.repeat != Repeat::kOnce; });
        if (recurring) {
            auto canonical = std::find_if(updated.begin(), updated.end(),
                [&](const Task& task) { return is_target(task) && task.repeat != Repeat::kOnce; });
            if (next_trigger_at > 0) canonical->trigger_at = next_trigger_at;
            canonical->enabled = true;
            auto override = std::find_if(updated.begin(), updated.end(),
                [&](const Task& task) {
                    return is_target(task) && task.repeat == Repeat::kOnce &&
                        task.trigger_at == snoozed_until;
                });
            if (override == updated.end()) {
                if (updated.size() >= kMaxTasks) {
                    throw std::runtime_error("稍后提醒无法写入已满的本地日程");
                }
                Task copy = *canonical;
                copy.id = updated_next_id++;
                copy.repeat = Repeat::kOnce;
                copy.weekdays.clear();
                copy.trigger_at = snoozed_until;
                updated.push_back(std::move(copy));
            }
        } else {
            found->trigger_at = snoozed_until;
            found->enabled = true;
        }
        for (auto& task : updated) {
            if (is_target(task)) {
                task.schedule_version = schedule_version;
                task.sync_state = SyncState::kSynced;
            }
        }
    } else {
        updated.erase(std::remove_if(updated.begin(), updated.end(), [&](const Task& task) {
            return is_target(task) && task.repeat == Repeat::kOnce;
        }), updated.end());
        auto canonical = std::find_if(updated.begin(), updated.end(),
            [&](const Task& task) { return is_target(task) && task.repeat != Repeat::kOnce; });
        if (canonical != updated.end()) {
            canonical->trigger_at = next_trigger_at;
            canonical->schedule_version = schedule_version;
            canonical->sync_state = SyncState::kSynced;
            canonical->enabled = true;
        }
    }
    tasks_ = std::move(updated);
    next_id_ = updated_next_id;
    action_revision_ = revision;
    return true;
}

std::vector<Task> Manager::List(KindFilter filter, bool enabled_only) const {
    std::vector<Task> result;
    for (const auto& task : tasks_) {
        if ((!enabled_only || task.enabled) && MatchesFilter(task, filter)) {
            result.push_back(task);
        }
    }
    return result;
}

size_t Manager::Clear(KindFilter filter) {
    const size_t old_size = tasks_.size();
    tasks_.erase(std::remove_if(tasks_.begin(), tasks_.end(),
                                [filter](const Task& task) {
                                    return MatchesFilter(task, filter);
                                }),
                 tasks_.end());
    return old_size - tasks_.size();
}

std::time_t Manager::NextOccurrence(const Task& task, std::time_t after) {
    if (task.repeat == Repeat::kOnce) {
        return 0;
    }
    std::tm original{};
    std::tm base{};
    if (localtime_r(&task.trigger_at, &original) == nullptr ||
        localtime_r(&after, &base) == nullptr) {
        throw std::runtime_error("无法计算下一次本地时间");
    }
    base.tm_hour = original.tm_hour;
    base.tm_min = original.tm_min;
    base.tm_sec = original.tm_sec;
    for (int offset = 0; offset <= 8; ++offset) {
        std::tm candidate = base;
        candidate.tm_mday += offset;
        candidate.tm_isdst = -1;
        const std::time_t value = std::mktime(&candidate);
        if (value <= after) {
            continue;
        }
        std::tm local{};
        localtime_r(&value, &local);
        const int weekday = IsoWeekday(local);
        if (task.repeat == Repeat::kDaily ||
            (task.repeat == Repeat::kWeekdays && weekday <= 5) ||
            (task.repeat == Repeat::kWeekends && weekday >= 6) ||
            (task.repeat == Repeat::kWeekly && ContainsDay(task.weekdays, weekday))) {
            return value;
        }
    }
    throw std::runtime_error("无法在一周内计算下一次触发时间");
}

TickResult Manager::Tick(std::time_t now, bool time_valid) {
    TickResult result;
    if (!time_valid) {
        return result;
    }
    for (auto it = tasks_.begin(); it != tasks_.end();) {
        if (!it->enabled) {
            ++it;
            continue;
        }
        if (it->trigger_at > now) {
            ++it;
            continue;
        }
        if (recovery_pending_ && it->repeat != Repeat::kOnce) {
            do {
                it->trigger_at = NextOccurrence(*it, now);
            } while (it->trigger_at <= now);
            result.changed = true;
            ++it;
            continue;
        }
        if (it->repeat == Repeat::kOnce) {
            if (!recovery_pending_ || now - it->trigger_at <= kOnceRecoveryWindowSeconds) {
                if (!RecordOccurrence(*it, it->trigger_at)) {
                    result.occurrence_outbox_full = true;
                    ++it;
                    continue;
                }
                result.triggered.push_back(*it);
            } else {
                result.missed.push_back(*it);
            }
            it = tasks_.erase(it);
            result.changed = true;
            continue;
        }
        if (!RecordOccurrence(*it, it->trigger_at)) {
            result.occurrence_outbox_full = true;
            ++it;
            continue;
        }
        result.triggered.push_back(*it);
        it->trigger_at = NextOccurrence(*it, now);
        result.changed = true;
        ++it;
    }
    recovery_pending_ = false;
    std::sort(result.triggered.begin(), result.triggered.end(), [](const Task& left, const Task& right) {
        if (left.trigger_at != right.trigger_at) return left.trigger_at < right.trigger_at;
        const bool left_alarm = left.kind == Kind::kAlarm;
        const bool right_alarm = right.kind == Kind::kAlarm;
        if (left_alarm != right_alarm) return left_alarm;
        return left.id < right.id;
    });
    return result;
}

const char* Manager::KindName(Kind kind) {
    if (kind == Kind::kAlarm) return "alarm";
    if (kind == Kind::kReminder) return "reminder";
    return "briefing";
}

const char* Manager::RepeatName(Repeat repeat) {
    switch (repeat) {
        case Repeat::kOnce: return "once";
        case Repeat::kDaily: return "daily";
        case Repeat::kWeekdays: return "weekdays";
        case Repeat::kWeekends: return "weekends";
        case Repeat::kWeekly: return "weekly";
    }
    throw std::invalid_argument("未知重复类型");
}

Kind Manager::ParseKind(const std::string& value) {
    if (value == "alarm") return Kind::kAlarm;
    if (value == "reminder") return Kind::kReminder;
    if (value == "briefing") return Kind::kBriefing;
    throw std::invalid_argument("kind必须是alarm/reminder/briefing之一");
}

KindFilter Manager::ParseKindFilter(const std::string& value) {
    if (value == "all") return KindFilter::kAll;
    if (value == "alarm") return KindFilter::kAlarm;
    if (value == "reminder") return KindFilter::kReminder;
    if (value == "briefing") return KindFilter::kBriefing;
    throw std::invalid_argument("kind必须是all/alarm/reminder/briefing之一");
}

Repeat Manager::ParseRepeat(const std::string& value) {
    if (value == "once") return Repeat::kOnce;
    if (value == "daily") return Repeat::kDaily;
    if (value == "weekdays") return Repeat::kWeekdays;
    if (value == "weekends") return Repeat::kWeekends;
    if (value == "weekly") return Repeat::kWeekly;
    throw std::invalid_argument("repeat必须是once/daily/weekdays/weekends/weekly之一");
}

std::string Manager::DescribeTask(const Task& task) {
    std::tm local{};
    if (localtime_r(&task.trigger_at, &local) == nullptr) {
        throw std::runtime_error("无法格式化任务本地时间");
    }
    char timestamp[20];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S", &local);
    const char* kind = task.kind == Kind::kAlarm ? "闹铃" :
        (task.kind == Kind::kReminder ? "提醒" : "每日简报");
    std::string repeat;
    switch (task.repeat) {
        case Repeat::kOnce: repeat = "单次"; break;
        case Repeat::kDaily: repeat = "每天"; break;
        case Repeat::kWeekdays: repeat = "工作日"; break;
        case Repeat::kWeekends: repeat = "周末"; break;
        case Repeat::kWeekly:
            repeat = "每周";
            for (size_t i = 0; i < task.weekdays.size(); ++i) {
                if (i > 0) repeat += "、";
                repeat += std::to_string(task.weekdays[i]);
            }
            break;
    }
    std::string result = "任务ID " + std::to_string(task.id) + "，类型" + kind +
        "，时间" + timestamp + "，重复规则" + repeat + "，内容“" + task.label + "”";
    if (task.kind == Kind::kBriefing) {
        result += "，模块" + task.sections;
        if (!task.location.empty()) result += "，地点" + task.location;
    }
    return result;
}

std::string Manager::DescribeCreation(const Task& task, std::time_t now) {
    std::tm trigger_local{};
    std::tm now_local{};
    localtime_r(&task.trigger_at, &trigger_local);
    localtime_r(&now, &now_local);

    std::string when;
    switch (task.repeat) {
        case Repeat::kDaily:
            when = "每天";
            break;
        case Repeat::kWeekdays:
            when = "工作日";
            break;
        case Repeat::kWeekends:
            when = "周末";
            break;
        case Repeat::kWeekly:
            when = "每周";
            for (size_t i = 0; i < task.weekdays.size(); ++i) {
                if (i != 0) when += "、";
                when += WeekdayChinese(task.weekdays[i] % 7);
            }
            break;
        case Repeat::kOnce: {
            std::tm trigger_day = trigger_local;
            std::tm current_day = now_local;
            trigger_day.tm_hour = trigger_day.tm_min = trigger_day.tm_sec = 0;
            current_day.tm_hour = current_day.tm_min = current_day.tm_sec = 0;
            trigger_day.tm_isdst = current_day.tm_isdst = -1;
            const int day_offset = static_cast<int>(
                std::difftime(std::mktime(&trigger_day), std::mktime(&current_day)) / 86400);
            if (day_offset == 0) {
                when = "今天";
            } else if (day_offset == 1) {
                when = "明天";
            } else if (day_offset == 2) {
                when = "后天";
            } else {
                when = std::to_string(trigger_local.tm_mon + 1) + "月" +
                    std::to_string(trigger_local.tm_mday) + "日";
            }
            break;
        }
    }
    when += FormatClock(task.trigger_at);
    if (task.kind == Kind::kAlarm) return "已设置" + when + "的闹铃。";
    if (task.kind == Kind::kBriefing) return "已设置" + when + "的每日简报。";
    return "已设置" + when + "提醒你" + task.label + "。";
}

bool Manager::ShouldSuggestRepeating(const std::vector<Task>& tasks,
                                     const Task& created) {
    if (created.repeat != Repeat::kOnce ||
        (created.kind != Kind::kAlarm && created.kind != Kind::kReminder)) {
        return false;
    }
    return std::any_of(tasks.begin(), tasks.end(), [&created](const Task& task) {
        return task.id != created.id && task.kind == created.kind &&
               task.repeat == Repeat::kOnce && task.label == created.label &&
               SameLocalClockOnDifferentDate(task.trigger_at, created.trigger_at);
    });
}

const Task* AlertQueue::StartNext() {
    if (active_ || pending_.empty()) return active_ ? &*active_ : nullptr;
    active_ = std::move(pending_.front());
    pending_.pop_front();
    return &*active_;
}

size_t AlertQueue::Remove(const std::string& schedule_uuid,
                          const std::string& source_schedule_id, bool local_source) {
    const auto old_size = pending_.size();
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
        [&](const Task& task) {
            return (!schedule_uuid.empty() && task.schedule_uuid == schedule_uuid) ||
                (local_source && task.schedule_uuid.empty() && task.local_source &&
                 task.source_schedule_id == source_schedule_id);
        }), pending_.end());
    return old_size - pending_.size();
}

std::optional<Task> AlertQueue::Stop() {
    auto stopped = active_;
    active_.reset();
    return stopped;
}

const Task& AlertQueue::current() const {
    if (!active_) throw std::runtime_error("当前没有活动提醒");
    return *active_;
}

void ReminderDeliverySequence::Begin() {
    state_ = ReminderDeliveryState::kWaitingForCue;
}

bool ReminderDeliverySequence::OnPlaybackDrained() {
    if (state_ != ReminderDeliveryState::kWaitingForCue) return false;
    state_ = ReminderDeliveryState::kWaitingForTts;
    return true;
}

bool ReminderDeliverySequence::OnTtsStarted() {
    if (state_ != ReminderDeliveryState::kWaitingForTts) return false;
    state_ = ReminderDeliveryState::kSpeaking;
    return true;
}

bool ReminderDeliverySequence::OnTtsStopped() {
    if (state_ != ReminderDeliveryState::kSpeaking) return false;
    state_ = ReminderDeliveryState::kInactive;
    return true;
}

bool ReminderDeliverySequence::CancelWaitingForTts() {
    if (state_ != ReminderDeliveryState::kWaitingForTts) return false;
    state_ = ReminderDeliveryState::kInactive;
    return true;
}

bool ReminderDeliverySequence::NeedsServerAbort() const {
    return state_ == ReminderDeliveryState::kWaitingForTts ||
           state_ == ReminderDeliveryState::kSpeaking;
}

}  // namespace schedule
