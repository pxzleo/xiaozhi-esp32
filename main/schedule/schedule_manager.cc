#include "schedule_manager.h"

#include <algorithm>
#include <stdexcept>

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
        ValidateBriefing(task.kind, task.sections, task.location);
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
