#include "schedule_manager.h"

#include <algorithm>
#include <stdexcept>

namespace schedule {

bool ShouldRestoreTemporaryVolume(uint32_t start_revision, uint32_t current_revision) {
    return start_revision == current_revision;
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
        (filter == KindFilter::kReminder && task.kind == Kind::kReminder);
}

}  // namespace

void Manager::Restore(std::vector<Task> tasks, uint32_t next_id) {
    if (tasks.size() > kMaxTasks) {
        throw std::invalid_argument("保存的定时任务超过16项");
    }
    uint32_t max_id = 0;
    for (const auto& task : tasks) {
        const auto label = AnalyzeUtf8(task.label);
        if (task.id == 0 || task.trigger_at <= 0 || !label.valid || label.count < 1 ||
            label.count > 80 || !label.has_non_whitespace) {
            throw std::invalid_argument("保存的定时任务字段无效");
        }
        if (task.repeat == Repeat::kWeekly && task.weekdays.empty()) {
            throw std::invalid_argument("保存的weekly任务缺少weekdays");
        }
        for (int day : task.weekdays) {
            if (day < 1 || day > 7) throw std::invalid_argument("保存的weekday超出1到7");
        }
        max_id = std::max(max_id, task.id);
    }
    if (next_id <= max_id) throw std::invalid_argument("保存的next_id没有递增");
    tasks_ = std::move(tasks);
    next_id_ = std::max<uint32_t>(next_id, 1);
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
    task.trigger_at = first_trigger;
    task.weekdays = request.weekdays;
    std::sort(task.weekdays.begin(), task.weekdays.end());
    task.weekdays.erase(std::unique(task.weekdays.begin(), task.weekdays.end()),
                        task.weekdays.end());
    tasks_.push_back(task);
    return task;
}

Task Manager::Snooze(const Task& source, int minutes, std::time_t now) {
    if (minutes < 1 || minutes > 60) {
        throw std::invalid_argument("稍后提醒分钟数必须在1到60之间");
    }
    CreateRequest request;
    request.kind = source.kind;
    request.repeat = Repeat::kOnce;
    request.label = source.label;
    request.delay_seconds = minutes * 60;
    return Create(request, now);
}

bool Manager::Delete(uint32_t id) {
    const auto old_size = tasks_.size();
    tasks_.erase(std::remove_if(tasks_.begin(), tasks_.end(),
                                [id](const Task& task) { return task.id == id; }),
                 tasks_.end());
    return tasks_.size() != old_size;
}

const Task* Manager::Find(uint32_t id) const {
    auto found = std::find_if(tasks_.begin(), tasks_.end(),
                              [id](const Task& task) { return task.id == id; });
    return found == tasks_.end() ? nullptr : &*found;
}

std::vector<Task> Manager::List(KindFilter filter) const {
    std::vector<Task> result;
    for (const auto& task : tasks_) {
        if (MatchesFilter(task, filter)) result.push_back(task);
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
                result.triggered.push_back(*it);
            } else {
                result.missed.push_back(*it);
            }
            it = tasks_.erase(it);
            result.changed = true;
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
        if (left.kind != right.kind) return left.kind == Kind::kAlarm;
        return left.id < right.id;
    });
    return result;
}

const char* Manager::KindName(Kind kind) { return kind == Kind::kAlarm ? "alarm" : "reminder"; }

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
    throw std::invalid_argument("kind必须是alarm或reminder");
}

KindFilter Manager::ParseKindFilter(const std::string& value) {
    if (value == "all") return KindFilter::kAll;
    if (value == "alarm") return KindFilter::kAlarm;
    if (value == "reminder") return KindFilter::kReminder;
    throw std::invalid_argument("kind必须是all/alarm/reminder之一");
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
    const char* kind = task.kind == Kind::kAlarm ? "闹铃" : "提醒";
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
    return "任务ID " + std::to_string(task.id) + "，类型" + kind + "，时间" + timestamp +
        "，重复规则" + repeat + "，内容“" + task.label + "”";
}

const Task* AlertQueue::StartNext() {
    if (active_ || pending_.empty()) return active_ ? &*active_ : nullptr;
    active_ = std::move(pending_.front());
    pending_.pop_front();
    return &*active_;
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

}  // namespace schedule
