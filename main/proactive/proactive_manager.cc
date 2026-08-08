#include "proactive_manager.h"

#include <algorithm>
#include <cstdio>
#include <stdexcept>

namespace proactive {
namespace {

int LocalDate(std::time_t now) {
    std::tm local{};
    if (localtime_r(&now, &local) == nullptr) throw std::runtime_error("无法读取本地日期");
    return (local.tm_year + 1900) * 10000 + (local.tm_mon + 1) * 100 + local.tm_mday;
}

void ValidateTopic(const std::string& topic) {
    static const std::set<std::string> kTopics{
        "reminder", "calendar", "weather", "music", "health", "habit", "system"};
    if (kTopics.count(topic) == 0) {
        throw std::invalid_argument("topic必须是受支持的主动主题");
    }
}

const std::string& PolicyTopic(const Event& event) {
    static const std::string kReminder = "reminder";
    static const std::string kHealth = "health";
    if (event.topic == "follow_up") return kReminder;
    if (event.topic == "health_critical") return kHealth;
    return event.topic;
}

void ValidateHealthKind(const std::string& kind) {
    if (!HealthTracker::IsSupportedKind(kind)) {
        throw std::invalid_argument("不支持的健康事件kind");
    }
}

void ValidatePersistentEvent(const Event& event) {
    if (event.event_id.empty() || event.event_id.size() > 96 ||
        event.dedupe_key.empty() || event.dedupe_key.size() > 96 ||
        event.reason.empty() || event.reason.size() > 64) {
        throw std::invalid_argument("持久主动事件文本字段超出限制");
    }
    if (event.topic == "follow_up") {
        if (!event.requires_response ||
            event.priority != Priority::kNormal ||
            event.reason != "schedule follow up" ||
            event.metadata.count("source_id") != 1 || event.metadata.size() > 2) {
            throw std::invalid_argument("持久追问事件schema无效");
        }
        for (const auto& [key, value] : event.metadata) {
            if ((key != "source_id" && key != "cue_played") || value.size() > 32) {
                throw std::invalid_argument("持久追问metadata无效");
            }
        }
        const auto& source_id = event.metadata.at("source_id");
        if (source_id.empty() || !std::all_of(source_id.begin(), source_id.end(),
                [](unsigned char value) { return value >= '0' && value <= '9'; }) ||
            (event.metadata.count("cue_played") != 0 &&
             event.metadata.at("cue_played") != "true")) {
            throw std::invalid_argument("持久追问metadata值无效");
        }
        return;
    }
    if (event.topic != "health" && event.topic != "health_critical") {
        throw std::invalid_argument("持久主动事件topic不受支持");
    }
    const auto kind = event.metadata.find("health_kind");
    const auto severity = event.metadata.find("severity");
    const auto recovered = event.metadata.find("recovered");
    if (event.requires_response || kind == event.metadata.end() ||
        severity == event.metadata.end() || recovered == event.metadata.end() ||
        !HealthTracker::IsSupportedKind(kind->second) ||
        (severity->second != "info" && severity->second != "warning" &&
         severity->second != "critical") ||
        (recovered->second != "true" && recovered->second != "false")) {
        throw std::invalid_argument("持久健康事件schema无效");
    }
    const Priority expected_priority = severity->second == "critical" ? Priority::kCritical :
        (severity->second == "warning" ? Priority::kHigh : Priority::kNormal);
    const std::string expected_reason = recovered->second == "true" ?
        "device health recovered" : "device health";
    if (event.priority != expected_priority || event.reason != expected_reason ||
        (expected_priority == Priority::kCritical) != (event.topic == "health_critical")) {
        throw std::invalid_argument("持久健康事件优先级或原因无效");
    }
    for (const auto& [key, value] : event.metadata) {
        const bool base = key == "health_kind" || key == "severity" || key == "recovered";
        const bool detail = key == "detail.error_code" || key == "detail.version" ||
            key == "detail.uptime_seconds" || key == "detail.disconnects_in_5m";
        if ((!base && !detail) || value.size() > 96) {
            throw std::invalid_argument("持久健康metadata无效");
        }
    }
}

}  // namespace

std::vector<uint8_t> StateCodec::Compress(const std::string& input) {
    uint32_t checksum = 2166136261u;
    for (uint8_t value : input) checksum = (checksum ^ value) * 16777619u;
    std::vector<uint8_t> output{'P', 'Z', '2', 0};
    auto append_u32 = [&output](uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8) {
            output.push_back(static_cast<uint8_t>(value >> shift));
        }
    };
    append_u32(static_cast<uint32_t>(input.size()));
    append_u32(checksum);
    size_t cursor = 0;
    while (cursor < input.size()) {
        const size_t flags_index = output.size();
        output.push_back(0);
        for (int bit = 0; bit < 8 && cursor < input.size(); ++bit) {
            size_t best_length = 0;
            size_t best_offset = 0;
            const size_t window_start = cursor > 65535 ? cursor - 65535 : 0;
            for (size_t candidate = window_start; candidate < cursor; ++candidate) {
                size_t length = 0;
                while (length < 255 && cursor + length < input.size() &&
                       input[candidate + length] == input[cursor + length]) {
                    ++length;
                }
                if (length >= 4 && length > best_length) {
                    best_length = length;
                    best_offset = cursor - candidate;
                }
            }
            if (best_length >= 4) {
                output[flags_index] |= static_cast<uint8_t>(1u << bit);
                output.push_back(static_cast<uint8_t>(best_offset));
                output.push_back(static_cast<uint8_t>(best_offset >> 8));
                output.push_back(static_cast<uint8_t>(best_length));
                cursor += best_length;
            } else {
                output.push_back(static_cast<uint8_t>(input[cursor++]));
            }
        }
    }
    return output;
}

std::string StateCodec::Decompress(const uint8_t* data, size_t size, size_t max_output) {
    if (data == nullptr || size < 12 || data[0] != 'P' || data[1] != 'Z' ||
        (data[2] != '1' && data[2] != '2') || data[3] != 0) {
        throw std::runtime_error("主动状态压缩头无效");
    }
    const bool legacy_pz1 = data[2] == '1';
    auto read_u32 = [data](size_t offset) {
        return static_cast<uint32_t>(data[offset]) |
            (static_cast<uint32_t>(data[offset + 1]) << 8) |
            (static_cast<uint32_t>(data[offset + 2]) << 16) |
            (static_cast<uint32_t>(data[offset + 3]) << 24);
    };
    const size_t expected_size = read_u32(4);
    const uint32_t expected_checksum = read_u32(8);
    if (expected_size == 0 || expected_size > max_output) {
        throw std::runtime_error("主动状态解压长度无效");
    }
    std::string output;
    output.reserve(expected_size);
    size_t cursor = 12;
    while (cursor < size && output.size() < expected_size) {
        const uint8_t flags = data[cursor++];
        for (int bit = 0; bit < 8 && output.size() < expected_size; ++bit) {
            if ((flags & (1u << bit)) != 0) {
                const size_t reference_bytes = legacy_pz1 ? 2 : 3;
                if (cursor + reference_bytes > size) {
                    throw std::runtime_error("主动状态压缩引用截断");
                }
                const size_t offset = legacy_pz1 ? data[cursor] :
                    (data[cursor] | (static_cast<size_t>(data[cursor + 1]) << 8));
                cursor += legacy_pz1 ? 1 : 2;
                const size_t length = data[cursor++];
                if (offset == 0 || length < (legacy_pz1 ? 3 : 4) ||
                    offset > output.size() ||
                    output.size() + length > expected_size) {
                    throw std::runtime_error("主动状态压缩引用无效");
                }
                for (size_t i = 0; i < length; ++i) {
                    output.push_back(output[output.size() - offset]);
                }
            } else {
                if (cursor >= size) throw std::runtime_error("主动状态压缩字面量截断");
                output.push_back(static_cast<char>(data[cursor++]));
            }
        }
    }
    if (output.size() != expected_size || cursor != size) {
        throw std::runtime_error("主动状态压缩数据长度不一致");
    }
    uint32_t checksum = 2166136261u;
    for (uint8_t value : output) checksum = (checksum ^ value) * 16777619u;
    if (checksum != expected_checksum) throw std::runtime_error("主动状态校验失败");
    return output;
}

void Manager::Restore(Config config, RuntimeState state) {
    const Mode effective_mode = config.mode == Mode::kTodaySilent ?
        config.mode_before_silent : config.mode;
    if (config.mode == Mode::kAggressive && config.daily_limit >= 0 && config.daily_limit <= 5) {
        config.daily_limit = 0;
    }
    if (config.mode == Mode::kTodaySilent) {
        if (config.daily_limit < 0 || config.daily_limit > 5) {
            throw std::invalid_argument("静默模式每日上限无效");
        }
        config.daily_limit = 0;
        if (config.mode_before_silent == Mode::kAggressive &&
            config.previous_daily_limit >= 0 && config.previous_daily_limit <= 5) {
            config.previous_daily_limit = 0;
        }
    } else {
        config.previous_daily_limit = config.daily_limit;
    }
    const int effective_limit = config.mode == Mode::kTodaySilent ?
        config.previous_daily_limit : config.daily_limit;
    const bool valid_limit = effective_mode == Mode::kAggressive ? effective_limit == 0 :
        (effective_mode == Mode::kConservative ? effective_limit == 1 :
         effective_limit >= 1 && effective_limit <= 5);
    if (!valid_limit) {
        throw std::invalid_argument("每日上限超过当前模式允许值");
    }
    if (config.quiet_start.has_value() != config.quiet_end.has_value()) {
        throw std::invalid_argument("安静时段开始和结束必须成对");
    }
    if ((config.quiet_start && (*config.quiet_start < 0 || *config.quiet_start >= 1440)) ||
        (config.quiet_end && (*config.quiet_end < 0 || *config.quiet_end >= 1440))) {
        throw std::invalid_argument("安静时段超出一天范围");
    }
    if (config.allowed_topics.size() + config.blocked_topics.size() > 7) {
        throw std::invalid_argument("保存的主动主题状态超过7项上限");
    }
    for (const auto& topic : config.allowed_topics) ValidateTopic(topic);
    for (const auto& topic : config.blocked_topics) ValidateTopic(topic);
    config_ = std::move(config);
    state_ = std::move(state);
    const auto now = std::time(nullptr);
    if (now > 1600000000) TrimCooldowns(now);
}

void Manager::TrimCooldowns(std::time_t now) {
    for (auto item = state_.last_delivered.begin(); item != state_.last_delivered.end();) {
        if (item->second < now - kCooldownSeconds) item = state_.last_delivered.erase(item);
        else ++item;
    }
    while (state_.last_delivered.size() > 5) {
        auto oldest = std::min_element(state_.last_delivered.begin(), state_.last_delivered.end(),
            [](const auto& left, const auto& right) { return left.second < right.second; });
        state_.last_delivered.erase(oldest);
    }
}

void Manager::Configure(Mode mode, std::optional<int> daily_limit,
                        std::optional<int> quiet_start, std::optional<int> quiet_end) {
    if (mode == Mode::kTodaySilent) {
        throw std::invalid_argument("today_silent只能通过mute工具设置");
    }
    if (quiet_start.has_value() != quiet_end.has_value()) {
        throw std::invalid_argument("quiet_start和quiet_end必须成对提供");
    }
    if ((quiet_start && (*quiet_start < 0 || *quiet_start >= 1440)) ||
        (quiet_end && (*quiet_end < 0 || *quiet_end >= 1440))) {
        throw std::invalid_argument("安静时段超出一天范围");
    }
    const int limit = daily_limit.value_or(DefaultLimit(mode));
    const bool valid_limit = mode == Mode::kAggressive ? limit == 0 :
        (mode == Mode::kConservative ? limit == 1 : limit >= 1 && limit <= 5);
    if (!valid_limit) {
        throw std::invalid_argument("daily_limit超过当前模式允许值");
    }
    config_.mode = mode;
    config_.mode_before_silent = mode;
    config_.silent_date = 0;
    config_.daily_limit = limit;
    config_.previous_daily_limit = limit;
    if (quiet_start) {
        config_.quiet_start = quiet_start;
        config_.quiet_end = quiet_end;
    }
}

void Manager::MuteToday(std::time_t now, bool time_valid) {
    if (!time_valid) throw std::runtime_error("设备时间尚未同步，不能设置今天安静");
    RefreshDate(now, true);
    if (config_.mode != Mode::kTodaySilent) {
        config_.mode_before_silent = config_.mode;
        config_.previous_daily_limit = config_.daily_limit;
    }
    config_.mode = Mode::kTodaySilent;
    config_.daily_limit = 0;
    config_.silent_date = LocalDate(now);
}

void Manager::AllowTopic(const std::string& topic) {
    ValidateTopic(topic);
    config_.blocked_topics.erase(topic);
    if (config_.allowed_topics.count(topic) == 0 &&
        config_.allowed_topics.size() + config_.blocked_topics.size() >= 7) {
        throw std::runtime_error("主动主题规则已达7项上限");
    }
    config_.allowed_topics.insert(topic);
}

void Manager::BlockTopic(const std::string& topic) {
    ValidateTopic(topic);
    config_.allowed_topics.erase(topic);
    if (config_.blocked_topics.count(topic) == 0 &&
        config_.allowed_topics.size() + config_.blocked_topics.size() >= 7) {
        throw std::runtime_error("主动主题规则已达7项上限");
    }
    config_.blocked_topics.insert(topic);
}

void Manager::RefreshDate(std::time_t now, bool time_valid) {
    if (!time_valid) return;
    TrimCooldowns(now);
    const int date = LocalDate(now);
    if (state_.budget_date != date) {
        state_.budget_date = date;
        state_.delivered_today = 0;
    }
    if (config_.mode == Mode::kTodaySilent && config_.silent_date != 0 &&
        config_.silent_date != date) {
        config_.mode = config_.mode_before_silent;
        config_.daily_limit = config_.previous_daily_limit;
        config_.silent_date = 0;
    }
}

bool Manager::IsQuiet(std::time_t now) const {
    if (!config_.quiet_start || !config_.quiet_end) return false;
    std::tm local{};
    if (localtime_r(&now, &local) == nullptr) return true;
    const int minute = local.tm_hour * 60 + local.tm_min;
    if (*config_.quiet_start == *config_.quiet_end) return true;
    if (*config_.quiet_start < *config_.quiet_end) {
        return minute >= *config_.quiet_start && minute < *config_.quiet_end;
    }
    return minute >= *config_.quiet_start || minute < *config_.quiet_end;
}

bool Manager::IsCritical(const Event& event) {
    return event.priority == Priority::kCritical || event.topic == "alarm" ||
        event.topic == "reminder" || event.topic == "health_critical";
}

bool Manager::ShouldDeliver(const Event& event, std::time_t now, bool time_valid) {
    RefreshDate(now, time_valid);
    if (event.expires_at > 0 && event.expires_at < now) return false;
    const bool critical = IsCritical(event);
    const auto& topic = PolicyTopic(event);
    if (!time_valid && !critical) return false;
    if (config_.blocked_topics.count(topic) != 0) return false;
    if (critical) return true;
    if (!config_.allowed_topics.empty() &&
        config_.allowed_topics.count(topic) == 0) return false;
    if (config_.mode == Mode::kTodaySilent || config_.mode == Mode::kConservative) return false;
    if (IsQuiet(now)) return false;
    auto previous = state_.last_delivered.find(topic);
    if (previous != state_.last_delivered.end() && now - previous->second < kCooldownSeconds) {
        return false;
    }
    return config_.mode == Mode::kAggressive ||
        state_.delivered_today < config_.daily_limit;
}

void Manager::RecordDelivered(const Event& event, std::time_t now, bool time_valid) {
    RefreshDate(now, time_valid);
    const auto& topic = PolicyTopic(event);
    if (state_.last_delivered.count(topic) == 0 &&
        state_.last_delivered.size() >= 5) {
        auto oldest = std::min_element(
            state_.last_delivered.begin(), state_.last_delivered.end(),
            [](const auto& left, const auto& right) { return left.second < right.second; });
        state_.last_delivered.erase(oldest);
    }
    state_.last_delivered[topic] = now;
    if (time_valid && !IsCritical(event)) ++state_.delivered_today;
}

Mode Manager::ParseMode(const std::string& value) {
    if (value == "conservative") return Mode::kConservative;
    if (value == "active") return Mode::kActive;
    if (value == "aggressive") return Mode::kAggressive;
    if (value == "today_silent") return Mode::kTodaySilent;
    throw std::invalid_argument("mode必须是conservative/active/aggressive/today_silent之一");
}

const char* Manager::ModeName(Mode mode) {
    switch (mode) {
        case Mode::kConservative: return "conservative";
        case Mode::kActive: return "active";
        case Mode::kAggressive: return "aggressive";
        case Mode::kTodaySilent: return "today_silent";
    }
    throw std::invalid_argument("未知主动模式");
}

int Manager::DefaultLimit(Mode mode) {
    if (mode == Mode::kConservative) return 1;
    if (mode == Mode::kActive) return 5;
    return 0;
}

int Manager::ParseClock(const std::string& value) {
    int hour = -1, minute = -1;
    char trailing = 0;
    if (std::sscanf(value.c_str(), "%d:%d%c", &hour, &minute, &trailing) != 2 ||
        value.size() != 5 || value[2] != ':' || hour < 0 || hour > 23 ||
        minute < 0 || minute > 59) {
        throw std::invalid_argument("时间必须是HH:MM格式");
    }
    return hour * 60 + minute;
}

std::string Manager::FormatClock(int minutes) {
    if (minutes < 0 || minutes >= 1440) throw std::invalid_argument("分钟数超出一天范围");
    char buffer[6];
    std::snprintf(buffer, sizeof(buffer), "%02d:%02d", minutes / 60, minutes % 60);
    return buffer;
}

void FollowUpStore::Restore(std::vector<FollowUp> items) {
    if (items.size() > kMaxItems) throw std::invalid_argument("保存的追问超过4项");
    for (const auto& item : items) {
        if (item.source_id == 0 || item.label.empty() || item.source_triggered_at <= 0 ||
            item.due_at <= item.source_triggered_at || item.expires_at < item.due_at) {
            throw std::invalid_argument("保存的追问字段无效");
        }
    }
    items_ = std::move(items);
}

void FollowUpStore::Schedule(uint32_t source_id, const std::string& label,
                             std::time_t source_triggered_at,
                             std::time_t scheduled_at) {
    if (source_id == 0 || label.empty() || source_triggered_at <= 0 || scheduled_at <= 0) {
        throw std::invalid_argument("追问来源无效");
    }
    items_.erase(std::remove_if(items_.begin(), items_.end(),
                                [source_id](const FollowUp& item) {
                                    return item.source_id == source_id;
                                }), items_.end());
    if (items_.size() >= kMaxItems) throw std::runtime_error("待确认提醒已达4项上限");
    items_.push_back({source_id, label, source_triggered_at,
                      scheduled_at + kDefaultDelaySeconds,
                      scheduled_at + kDefaultDelaySeconds + kLateGraceSeconds, false});
}

std::vector<FollowUp> FollowUpStore::PendingDue(std::time_t now) {
    DropExpired(now);
    std::vector<FollowUp> result;
    for (const auto& item : items_) {
        if (!item.asked && item.due_at <= now) {
            result.push_back(item);
        }
    }
    return result;
}

void FollowUpStore::MarkAsked(uint32_t source_id, std::time_t due_at) {
    auto found = std::find_if(items_.begin(), items_.end(),
                              [source_id, due_at](const FollowUp& item) {
                                  return item.source_id == source_id && item.due_at == due_at;
                              });
    if (found == items_.end()) throw std::runtime_error("待确认提醒已不存在");
    if (found->asked) throw std::runtime_error("该提醒已经追问过");
    found->asked = true;
}

size_t FollowUpStore::FindRecent(std::time_t now) const {
    size_t found = items_.size();
    std::time_t latest = 0;
    bool ambiguous = false;
    for (size_t i = 0; i < items_.size(); ++i) {
        if (items_[i].expires_at < now) continue;
        if (items_[i].source_triggered_at > latest) {
            found = i;
            latest = items_[i].source_triggered_at;
            ambiguous = false;
        } else if (items_[i].source_triggered_at == latest) {
            ambiguous = true;
        }
    }
    if (found == items_.size()) throw std::runtime_error("当前没有未决的提醒确认");
    if (ambiguous) throw std::runtime_error("有多个同一时间的未决提醒，请说明具体内容");
    return found;
}

FollowUp FollowUpStore::CompleteRecent(std::time_t now) {
    DropExpired(now);
    const size_t index = FindRecent(now);
    FollowUp result = items_[index];
    items_.erase(items_.begin() + index);
    return result;
}

FollowUp FollowUpStore::DelayRecent(int minutes, std::time_t now) {
    if (minutes < 1 || minutes > 60) throw std::invalid_argument("分钟数必须在1到60之间");
    DropExpired(now);
    const size_t index = FindRecent(now);
    items_[index].due_at = now + minutes * 60;
    items_[index].expires_at = items_[index].due_at + kLateGraceSeconds;
    items_[index].asked = false;
    return items_[index];
}

FollowUp FollowUpStore::DismissRecent(std::time_t now) { return CompleteRecent(now); }

size_t FollowUpStore::DropExpired(std::time_t now) {
    const size_t before = items_.size();
    items_.erase(std::remove_if(items_.begin(), items_.end(),
                                [now](const FollowUp& item) { return item.expires_at < now; }),
                 items_.end());
    return before - items_.size();
}

void HealthTracker::Restore(std::map<std::string, Record> records) {
    if (records.size() > 4) throw std::invalid_argument("保存的健康状态超过4项");
    for (const auto& item : records) ValidateHealthKind(item.first);
    records_ = std::move(records);
}

std::optional<HealthEvent> HealthTracker::Raise(
    const std::string& kind, Severity severity, std::time_t now,
    std::map<std::string, std::string> details) {
    ValidateHealthKind(kind);
    auto& record = records_[kind];
    if (record.active) return std::nullopt;
    if (record.last_changed_at > 0 && now - record.last_changed_at < kCooldownSeconds) {
        return std::nullopt;
    }
    if (record.dedupe_key.empty()) record.dedupe_key = "health:" + kind;
    record.active = true;
    record.last_changed_at = now;
    const Priority priority = severity == Severity::kCritical ? Priority::kCritical :
        (severity == Severity::kWarning ? Priority::kHigh : Priority::kNormal);
    Event event{kind + ":" + std::to_string(now),
                severity == Severity::kCritical ? "health_critical" : "health",
                priority, "device health", now, now + 24 * 60 * 60,
                record.dedupe_key, false, {}};
    return HealthEvent{event, kind, severity, now, false, std::move(details)};
}

std::optional<HealthEvent> HealthTracker::Recover(const std::string& kind, std::time_t now) {
    ValidateHealthKind(kind);
    auto found = records_.find(kind);
    if (found == records_.end() || !found->second.active) return std::nullopt;
    found->second.active = false;
    found->second.last_changed_at = now;
    Event event{kind + ":recovered:" + std::to_string(now), "health", Priority::kNormal,
                "device health recovered", now, now + 24 * 60 * 60,
                found->second.dedupe_key, false, {}};
    return HealthEvent{event, kind, Severity::kInfo, now, true, {}};
}

bool HealthTracker::IsSupportedKind(const std::string& kind) {
    return kind == "network_flapping" || kind == "time_unsynchronized" ||
           kind == "ota_update_available" || kind == "audio_decode_failed";
}

const char* HealthTracker::SeverityName(Severity severity) {
    switch (severity) {
        case Severity::kInfo: return "info";
        case Severity::kWarning: return "warning";
        case Severity::kCritical: return "critical";
    }
    throw std::invalid_argument("未知健康严重级别");
}

void DurableQueue::Restore(std::vector<Event> items) {
    if (items.size() > kMaxItems) throw std::invalid_argument("保存的主动队列超过8项");
    items_.clear();
    for (auto& item : items) Push(std::move(item));
}

std::optional<Event> DurableQueue::Push(Event event) {
    ValidatePersistentEvent(event);
    auto found = std::find_if(items_.begin(), items_.end(), [&event](const Event& item) {
        return item.event_id == event.event_id || item.dedupe_key == event.dedupe_key;
    });
    if (found != items_.end()) {
        *found = std::move(event);
        return std::nullopt;
    }
    std::optional<Event> evicted;
    if (items_.size() >= kMaxItems) {
        auto importance = [](const Event& item) {
            if (item.metadata.count("recovered") != 0 &&
                item.metadata.at("recovered") == "true") return 10;
            return static_cast<int>(item.priority) * 2 +
                (item.metadata.count("health_kind") != 0 ? 1 : 0);
        };
        const bool incoming_health = event.metadata.count("health_kind") != 0;
        auto lowest = items_.end();
        for (auto item = items_.begin(); item != items_.end(); ++item) {
            if (item->metadata.count("health_kind") != 0 ||
                item->priority == Priority::kCritical) {
                continue;
            }
            if (lowest == items_.end() || importance(*item) < importance(*lowest) ||
                (importance(*item) == importance(*lowest) &&
                 item->created_at < lowest->created_at)) {
                lowest = item;
            }
        }
        if (lowest == items_.end() ||
            (!incoming_health && importance(event) <= importance(*lowest))) {
            throw std::runtime_error("主动事件队列已满且没有可淘汰的低优先级事件");
        }
        evicted = std::move(*lowest);
        items_.erase(lowest);
    }
    items_.push_back(std::move(event));
    return evicted;
}

std::optional<Event> DurableQueue::PopNext(std::time_t now) {
    DropExpired(now);
    if (items_.empty()) return std::nullopt;
    auto best = std::max_element(items_.begin(), items_.end(), [](const Event& left,
                                                                  const Event& right) {
        if (left.priority != right.priority) return left.priority < right.priority;
        return left.created_at > right.created_at;
    });
    Event result = *best;
    items_.erase(best);
    return result;
}

size_t DurableQueue::RemoveByDedupeKey(const std::string& dedupe_key) {
    const size_t before = items_.size();
    items_.erase(std::remove_if(items_.begin(), items_.end(),
                                [&dedupe_key](const Event& event) {
                                    return event.dedupe_key == dedupe_key;
                                }), items_.end());
    return before - items_.size();
}

size_t DurableQueue::DropExpired(std::time_t now) {
    const size_t before = items_.size();
    items_.erase(std::remove_if(items_.begin(), items_.end(), [now](const Event& item) {
        return item.expires_at > 0 && item.expires_at < now;
    }), items_.end());
    return before - items_.size();
}

void RetryBackoff::OnFailure(int64_t now_us) {
    retry_after_us_ = now_us + static_cast<int64_t>(delay_seconds_) * 1000000;
    delay_seconds_ = std::min(delay_seconds_ * 2, 300);
}

void RetryBackoff::OnSuccess() {
    retry_after_us_ = 0;
    delay_seconds_ = 5;
}

}  // namespace proactive
