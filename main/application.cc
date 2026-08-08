#include "application.h"
#include "assets.h"
#include "assets/lang_config.h"
#include "audio_codec.h"
#include "board.h"
#include "display.h"
#include "mcp_server.h"
#include "mqtt_protocol.h"
#include "settings.h"
#include "system_info.h"
#include "text_glyph_payload.h"
#include "websocket_protocol.h"

#include <driver/gpio.h>
#include <esp_log.h>
#include <nvs.h>
#include <arpa/inet.h>
#include <cJSON.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <string_view>

#define TAG "Application"

namespace {

constexpr int kScheduleAlertMinimumVolume = 80;
constexpr int kReminderCueRepeats = 2;
constexpr int kReminderTtsStartTimeoutSeconds = 15;
constexpr int kBriefingTtsStartTimeoutSeconds = 30;

struct ToolStatusTranslation {
    std::string_view keyword;
    const char* message;
};

// The server uses a leading "%" to mark display-only tool-call notifications,
// for example "% web_search". Keep raw function names out of the user-facing UI.
std::string LocalizeToolStatusMessage(const char* content) {
    if (content == nullptr) {
        return {};
    }

    std::string message(content);
    const auto marker = message.find_first_not_of(" \t\r\n");
    if (marker == std::string::npos || message[marker] != '%') {
        return message;
    }

    std::string tool_name = message.substr(marker + 1);
    const auto name_start = tool_name.find_first_not_of(" \t\r\n");
    if (name_start != std::string::npos) {
        tool_name.erase(0, name_start);
    }
    for (char& ch : tool_name) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }

    static constexpr ToolStatusTranslation kTranslations[] = {
        {"call_device", "呼叫设备"},  {"change_role", "切换角色"},
        {"hass_", "控制智能家居"},    {"home_assistant", "控制智能家居"},
        {"exit_intent", "结束对话"}, {"lunar", "查询日期"},
        {"music", "播放音乐"},       {"song", "播放音乐"},
        {"search", "开始搜索"},      {"find", "开始搜索"},
        {"weather", "查询天气"},     {"forecast", "查询天气"},
        {"news", "查询资讯"},        {"calendar", "查询日程"},
        {"schedule", "查询日程"},    {"reminder", "设置提醒"},
        {"alarm", "设置提醒"},       {"timer", "设置计时器"},
        {"email", "处理邮件"},       {"mail", "处理邮件"},
        {"navigation", "查询路线"},  {"route", "查询路线"},
        {"map", "查询地点"},         {"location", "查询地点"},
        {"camera", "查看画面"},      {"photo", "拍摄照片"},
        {"image", "查看图片"},       {"vision", "查看画面"},
        {"device_status", "检查设备状态"},
        {"system_info", "查看设备信息"},
        {"volume", "调整音量"},      {"brightness", "调整亮度"},
        {"backlight", "调整亮度"},   {"theme", "切换主题"},
        {"screen", "调整屏幕"},      {"display", "调整屏幕"},
        {"light", "控制灯光"},       {"lamp", "控制灯光"},
        {"led", "控制灯光"},         {"reboot", "重启设备"},
        {"restart", "重启设备"},     {"upgrade", "升级设备"},
        {"update", "更新设备"},      {"download", "下载内容"},
        {"upload", "上传内容"},      {"phone", "拨打电话"},
        {"message", "发送消息"},
        {"time", "查询时间"},        {"date", "查询日期"},
    };

    for (const auto& translation : kTranslations) {
        if (tool_name.find(translation.keyword) != std::string::npos) {
            return translation.message;
        }
    }
    return "正在处理，请稍候";
}

size_t Utf8CodePointCount(const char* value) {
    size_t count = 0;
    for (const auto* p = reinterpret_cast<const unsigned char*>(value); *p != 0; ++p) {
        if ((*p & 0xC0) != 0x80) {
            ++count;
        }
    }
    return count;
}

bool IsAsciiString(const char* value, bool (*predicate)(unsigned char)) {
    if (value == nullptr || *value == '\0') {
        return false;
    }
    for (const auto* p = reinterpret_cast<const unsigned char*>(value); *p != 0; ++p) {
        if (!predicate(*p)) {
            return false;
        }
    }
    return true;
}

bool IsHex(unsigned char value) { return std::isxdigit(value) != 0; }
bool IsDigit(unsigned char value) { return std::isdigit(value) != 0; }

std::string JsonString(cJSON* json) {
    char* raw = cJSON_PrintUnformatted(json);
    if (raw == nullptr) {
        cJSON_Delete(json);
        throw std::runtime_error("JSON序列化失败");
    }
    std::string result(raw);
    cJSON_free(raw);
    cJSON_Delete(json);
    return result;
}

std::time_t ParseLocalDateTime(const std::string& value) {
    std::tm local{};
    char tail = 0;
    if (sscanf(value.c_str(), "%d-%d-%dT%d:%d:%d%c", &local.tm_year, &local.tm_mon,
               &local.tm_mday, &local.tm_hour, &local.tm_min, &local.tm_sec, &tail) != 6) {
        throw std::invalid_argument("trigger_at必须是本地时间YYYY-MM-DDTHH:MM:SS");
    }
    local.tm_year -= 1900;
    local.tm_mon -= 1;
    local.tm_isdst = -1;
    const std::time_t timestamp = std::mktime(&local);
    if (timestamp <= 0) {
        throw std::invalid_argument("trigger_at不是有效本地时间");
    }
    std::tm check{};
    localtime_r(&timestamp, &check);
    char formatted[20];
    strftime(formatted, sizeof(formatted), "%Y-%m-%dT%H:%M:%S", &check);
    if (value != formatted) {
        throw std::invalid_argument("trigger_at不是有效本地时间");
    }
    return timestamp;
}

std::vector<int> ParseWeekdays(const std::string& value) {
    std::vector<int> days;
    if (value.empty()) return days;
    std::istringstream input(value);
    std::string part;
    while (std::getline(input, part, ',')) {
        if (part.size() != 1 || part[0] < '1' || part[0] > '7') {
            throw std::invalid_argument("weekdays必须是1到7的逗号分隔列表，例如1,3,5");
        }
        days.push_back(part[0] - '0');
    }
    return days;
}

std::string FormatLocalDateTime(std::time_t timestamp) {
    std::tm local{};
    localtime_r(&timestamp, &local);
    char formatted[20];
    strftime(formatted, sizeof(formatted), "%Y-%m-%dT%H:%M:%S", &local);
    return formatted;
}

cJSON* ScheduleTaskJson(const schedule::Task& task) {
    cJSON* json = cJSON_CreateObject();
    cJSON_AddNumberToObject(json, "id", task.id);
    cJSON_AddStringToObject(json, "kind", schedule::Manager::KindName(task.kind));
    cJSON_AddStringToObject(json, "repeat", schedule::Manager::RepeatName(task.repeat));
    cJSON_AddStringToObject(json, "label", task.label.c_str());
    if (task.kind == schedule::Kind::kBriefing) {
        cJSON_AddStringToObject(json, "sections", task.sections.c_str());
        cJSON_AddStringToObject(json, "location", task.location.c_str());
    }
    cJSON_AddStringToObject(json, "trigger_at", FormatLocalDateTime(task.trigger_at).c_str());
    if (!task.weekdays.empty()) {
        cJSON* weekdays = cJSON_AddArrayToObject(json, "weekdays");
        for (int day : task.weekdays) cJSON_AddItemToArray(weekdays, cJSON_CreateNumber(day));
    }
    return json;
}

const char* PriorityName(proactive::Priority priority) {
    switch (priority) {
        case proactive::Priority::kLow: return "low";
        case proactive::Priority::kNormal: return "normal";
        case proactive::Priority::kHigh: return "high";
        case proactive::Priority::kCritical: return "critical";
    }
    throw std::invalid_argument("未知主动事件优先级");
}

proactive::Priority ParsePriority(const char* value) {
    if (strcmp(value, "low") == 0) return proactive::Priority::kLow;
    if (strcmp(value, "normal") == 0) return proactive::Priority::kNormal;
    if (strcmp(value, "high") == 0) return proactive::Priority::kHigh;
    if (strcmp(value, "critical") == 0) return proactive::Priority::kCritical;
    throw std::invalid_argument("NVS中的主动事件优先级无效");
}

cJSON* ProactiveEventJson(const proactive::Event& event) {
    cJSON* json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "event_id", event.event_id.c_str());
    cJSON_AddStringToObject(json, "topic", event.topic.c_str());
    cJSON_AddStringToObject(json, "priority", PriorityName(event.priority));
    cJSON_AddStringToObject(json, "reason", event.reason.c_str());
    cJSON_AddNumberToObject(json, "created_at", event.created_at);
    cJSON_AddNumberToObject(json, "expires_at", event.expires_at);
    cJSON_AddStringToObject(json, "dedupe_key", event.dedupe_key.c_str());
    cJSON_AddBoolToObject(json, "requires_response", event.requires_response);
    cJSON* metadata = cJSON_AddObjectToObject(json, "metadata");
    for (const auto& [key, value] : event.metadata) {
        cJSON_AddStringToObject(metadata, key.c_str(), value.c_str());
    }
    return json;
}

proactive::Event ParseProactiveEvent(cJSON* json) {
    auto event_id = cJSON_GetObjectItem(json, "event_id");
    auto topic = cJSON_GetObjectItem(json, "topic");
    auto priority = cJSON_GetObjectItem(json, "priority");
    auto reason = cJSON_GetObjectItem(json, "reason");
    auto created_at = cJSON_GetObjectItem(json, "created_at");
    auto expires_at = cJSON_GetObjectItem(json, "expires_at");
    auto dedupe_key = cJSON_GetObjectItem(json, "dedupe_key");
    auto requires_response = cJSON_GetObjectItem(json, "requires_response");
    auto metadata = cJSON_GetObjectItem(json, "metadata");
    if (!cJSON_IsString(event_id) || !cJSON_IsString(topic) || !cJSON_IsString(priority) ||
        !cJSON_IsString(reason) || !cJSON_IsNumber(created_at) ||
        !cJSON_IsNumber(expires_at) || !cJSON_IsString(dedupe_key) ||
        !cJSON_IsBool(requires_response) || !cJSON_IsObject(metadata)) {
        throw std::runtime_error("NVS中的主动事件字段无效");
    }
    proactive::Event event{event_id->valuestring, topic->valuestring,
                           ParsePriority(priority->valuestring), reason->valuestring,
                           static_cast<std::time_t>(created_at->valuedouble),
                           static_cast<std::time_t>(expires_at->valuedouble),
                           dedupe_key->valuestring, cJSON_IsTrue(requires_response), {}};
    cJSON* entry = nullptr;
    cJSON_ArrayForEach(entry, metadata) {
        if (!cJSON_IsString(entry)) throw std::runtime_error("NVS中的主动事件metadata无效");
        event.metadata[entry->string] = entry->valuestring;
    }
    if (event.metadata.count("source_id") != 0) {
        if (event.metadata.at("source_id").empty() ||
            !std::all_of(event.metadata.at("source_id").begin(),
                         event.metadata.at("source_id").end(),
                         [](unsigned char value) { return std::isdigit(value) != 0; })) {
            throw std::runtime_error("NVS中的追问事件metadata无效");
        }
    }
    if (event.metadata.count("health_kind") != 0) {
        const auto severity = event.metadata.find("severity");
        const auto recovered = event.metadata.find("recovered");
        if (event.metadata.at("health_kind").empty() || severity == event.metadata.end() ||
            recovered == event.metadata.end() ||
            (severity->second != "info" && severity->second != "warning" &&
             severity->second != "critical") ||
            (recovered->second != "true" && recovered->second != "false")) {
            throw std::runtime_error("NVS中的健康事件metadata无效");
        }
    }
    return event;
}

cJSON* ProactiveConfigJson(const proactive::Config& config,
                           const proactive::RuntimeState& state) {
    cJSON* json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "mode", proactive::Manager::ModeName(config.mode));
    cJSON_AddStringToObject(json, "mode_before_silent",
                            proactive::Manager::ModeName(config.mode_before_silent));
    cJSON_AddNumberToObject(json, "silent_date", config.silent_date);
    cJSON_AddNumberToObject(json, "daily_limit", config.daily_limit);
    if (config.quiet_start) {
        cJSON_AddNumberToObject(json, "quiet_start", *config.quiet_start);
        cJSON_AddNumberToObject(json, "quiet_end", *config.quiet_end);
    }
    cJSON* allowed = cJSON_AddArrayToObject(json, "allowed_topics");
    for (const auto& topic : config.allowed_topics) {
        cJSON_AddItemToArray(allowed, cJSON_CreateString(topic.c_str()));
    }
    cJSON* blocked = cJSON_AddArrayToObject(json, "blocked_topics");
    for (const auto& topic : config.blocked_topics) {
        cJSON_AddItemToArray(blocked, cJSON_CreateString(topic.c_str()));
    }
    cJSON_AddNumberToObject(json, "budget_date", state.budget_date);
    cJSON_AddNumberToObject(json, "delivered_today", state.delivered_today);
    cJSON* cooldowns = cJSON_AddObjectToObject(json, "last_delivered");
    for (const auto& [topic, timestamp] : state.last_delivered) {
        cJSON_AddNumberToObject(cooldowns, topic.c_str(), timestamp);
    }
    return json;
}

std::string ResponseEnvelope(const std::string& response, cJSON* data) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "action", "RESPONSE");
    cJSON_AddStringToObject(root, "response", response.c_str());
    cJSON_AddItemToObject(root, "data", data);
    return JsonString(root);
}

const char* KindFilterChinese(schedule::KindFilter filter) {
    if (filter == schedule::KindFilter::kAlarm) return "闹铃";
    if (filter == schedule::KindFilter::kReminder) return "提醒";
    if (filter == schedule::KindFilter::kBriefing) return "每日简报";
    return "闹铃、提醒和每日简报";
}

bool ParseLyricsStart(const cJSON* params, netease_music::LyricsPlayback& playback) {
    auto version = cJSON_GetObjectItem(params, "version");
    auto playback_id = cJSON_GetObjectItem(params, "playback_id");
    auto track = cJSON_GetObjectItem(params, "track");
    auto available = cJSON_GetObjectItem(params, "available");
    auto lines = cJSON_GetObjectItem(params, "lines");
    if (!cJSON_IsNumber(version) || version->valueint != 1 ||
        !cJSON_IsString(playback_id) || strlen(playback_id->valuestring) != 32 ||
        !IsAsciiString(playback_id->valuestring, IsHex) || !cJSON_IsObject(track) ||
        !cJSON_IsBool(available) || !cJSON_IsArray(lines)) {
        return false;
    }
    auto track_id = cJSON_GetObjectItem(track, "id");
    auto title = cJSON_GetObjectItem(track, "title");
    auto artists = cJSON_GetObjectItem(track, "artists");
    if (!cJSON_IsString(track_id) || !IsAsciiString(track_id->valuestring, IsDigit) ||
        !cJSON_IsString(title) || Utf8CodePointCount(title->valuestring) > 200 ||
        !cJSON_IsArray(artists) || cJSON_GetArraySize(artists) > 32) {
        return false;
    }
    playback.playback_id = playback_id->valuestring;
    playback.track_id = track_id->valuestring;
    playback.title = title->valuestring;
    playback.available = cJSON_IsTrue(available);
    for (int i = 0; i < cJSON_GetArraySize(artists); ++i) {
        auto artist = cJSON_GetArrayItem(artists, i);
        if (!cJSON_IsString(artist) || Utf8CodePointCount(artist->valuestring) > 200) {
            return false;
        }
        if (!playback.artists.empty()) {
            playback.artists += " / ";
        }
        playback.artists += artist->valuestring;
    }
    const int line_count = cJSON_GetArraySize(lines);
    if (line_count > 500 || (!playback.available && line_count != 0)) {
        return false;
    }
    size_t total_characters = 0;
    uint32_t previous_start = 0;
    for (int i = 0; i < line_count; ++i) {
        auto line = cJSON_GetArrayItem(lines, i);
        auto start = cJSON_GetObjectItem(line, "start_ms");
        auto text = cJSON_GetObjectItem(line, "text");
        if (!cJSON_IsObject(line) || !cJSON_IsNumber(start) || start->valuedouble < 0 ||
            start->valuedouble > std::numeric_limits<uint32_t>::max() ||
            std::floor(start->valuedouble) != start->valuedouble || !cJSON_IsString(text)) {
            return false;
        }
        const uint32_t start_ms = static_cast<uint32_t>(start->valuedouble);
        const size_t characters = Utf8CodePointCount(text->valuestring);
        if ((i > 0 && start_ms < previous_start) || characters > 200 ||
            total_characters + characters > 32000) {
            return false;
        }
        playback.lines.push_back({start_ms, text->valuestring});
        previous_start = start_ms;
        total_characters += characters;
    }
    return true;
}

}  // namespace

Application::Application() {
    event_group_ = xEventGroupCreate();
    proactive_connection_done_ = xSemaphoreCreateBinary();
    if (proactive_connection_done_ == nullptr) {
        vEventGroupDelete(event_group_);
        event_group_ = nullptr;
        throw std::runtime_error("无法创建主动建链完成信号量");
    }

#if CONFIG_USE_DEVICE_AEC && CONFIG_USE_SERVER_AEC
#error "CONFIG_USE_DEVICE_AEC and CONFIG_USE_SERVER_AEC cannot be enabled at the same time"
#elif CONFIG_USE_DEVICE_AEC
    aec_mode_ = kAecOnDeviceSide;
#elif CONFIG_USE_SERVER_AEC
    aec_mode_ = kAecOnServerSide;
#else
    aec_mode_ = kAecOff;
#endif

    esp_timer_create_args_t clock_timer_args = {.callback =
                                                    [](void* arg) {
                                                        Application* app = (Application*)arg;
                                                        xEventGroupSetBits(app->event_group_,
                                                                           MAIN_EVENT_CLOCK_TICK);
                                                    },
                                                .arg = this,
                                                .dispatch_method = ESP_TIMER_TASK,
                                                .name = "clock_timer",
                                                .skip_unhandled_events = true};
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);
}

Application::~Application() {
    proactive_shutdown_.store(true);
    proactive_shutdown_token_->store(true);
    if (proactive_connection_busy_.load(std::memory_order_acquire)) {
        xSemaphoreTake(proactive_connection_done_, portMAX_DELAY);
        proactive_connection_busy_.store(false, std::memory_order_release);
    }
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    vEventGroupDelete(event_group_);
    vSemaphoreDelete(proactive_connection_done_);
}

bool Application::SetDeviceState(DeviceState state) { return state_machine_.TransitionTo(state); }

void Application::Initialize() {
    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);

    // Setup the display
    auto display = board.GetDisplay();
    display->SetupUI();
    // Print board name/version info
    display->SetChatMessage("system", SystemInfo::GetUserAgent().c_str());

    // Setup the audio service
    auto codec = board.GetAudioCodec();
    audio_service_.Initialize(codec);
    audio_service_.Start();

    AudioServiceCallbacks callbacks;
    callbacks.on_send_queue_available = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
    };
    callbacks.on_wake_word_detected = [this](const std::string& wake_word) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED);
    };
    callbacks.on_vad_change = [this](bool speaking) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE);
    };
    callbacks.on_barge_in_detected = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_BARGE_IN_DETECTED);
    };
    callbacks.on_playback_drained = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_PLAYBACK_DRAINED);
    };
    callbacks.on_critical_error = [this](const std::string& kind, int error_code,
                                         bool recovered) {
        Schedule([this, kind, error_code, recovered]() {
            if (recovered) {
                if (auto event = health_tracker_.Recover(kind, std::time(nullptr))) {
                    QueueHealthEvent(*event);
                }
            } else if (auto event = health_tracker_.Raise(
                           kind, proactive::Severity::kCritical, std::time(nullptr),
                           {{"error_code", std::to_string(error_code)}})) {
                QueueHealthEvent(*event);
            }
        });
    };
    callbacks.on_pcm_rendered = [this](uint32_t generation, size_t samples,
                                       uint32_t sample_rate, size_t buffered_samples) {
        auto update = netease_lyrics_.OnPcmRendered(generation, samples, sample_rate,
                                                    buffered_samples);
        if (update.has_value()) {
            Schedule([window = std::move(*update)]() {
                Board::GetInstance().GetDisplay()->UpdateNeteaseMusicLyrics(
                    window.previous, window.current, window.next);
            });
        }
    };
    audio_service_.SetCallbacks(callbacks);

    // Add state change listeners
    state_machine_.AddStateChangeListener([this](DeviceState old_state, DeviceState new_state) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
    });

    // Start the clock timer to update the status bar
    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    // Add MCP common tools (only once during initialization)
    try {
        LoadSchedules();
    } catch (const std::exception& error) {
        ESP_LOGE(TAG, "Failed to load schedules; quarantining damaged data: %s", error.what());
        Settings settings("schedule", true);
        settings.SetString("last_load_error", error.what());
        settings.SetInt("chunks", 0);
        settings.EraseKey("tasks");
        for (int i = 0; i < 8; ++i) settings.EraseKey("data" + std::to_string(i));
        schedule_manager_.Restore({}, 1);
    }
    try {
        LoadProactive();
    } catch (const std::exception& error) {
        ESP_LOGE(TAG, "Failed to load proactive state; quarantining damaged data: %s",
                 error.what());
        Settings settings("proactive", true);
        settings.SetString("last_error", error.what());
        settings.SetInt("chunks", 0);
        for (int i = 0; i < 16; ++i) settings.EraseKey("data" + std::to_string(i));
        proactive_manager_.Restore({}, {});
        schedule_follow_ups_.Restore({});
        health_tracker_.Restore({});
        proactive_queue_.Restore({});
    }
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddCommonTools();
    mcp_server.AddUserOnlyTools();

    // Set network event callback for UI updates and network state handling
    board.SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
        auto display = Board::GetInstance().GetDisplay();

        switch (event) {
            case NetworkEvent::Scanning:
                display->ShowNotification(Lang::Strings::SCANNING_WIFI, 30000);
                break;
            case NetworkEvent::Connecting: {
                if (data.empty()) {
                    // Cellular network - registering without carrier info yet
                    display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                } else {
                    // WiFi or cellular with carrier info
                    std::string msg = Lang::Strings::CONNECT_TO;
                    msg += data;
                    msg += "...";
                    display->ShowNotification(msg.c_str(), 30000);
                }
                break;
            }
            case NetworkEvent::Connected: {
                std::string msg = Lang::Strings::CONNECTED_TO;
                msg += data;
                display->ShowNotification(msg.c_str(), 30000);
                if (!network_connected_.exchange(true)) {
                    xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_CONNECTED);
                }
                break;
            }
            case NetworkEvent::Disconnected: {
                if (network_connected_.exchange(false)) {
                    std::lock_guard<std::mutex> lock(network_health_mutex_);
                    network_disconnect_us_.push_back(esp_timer_get_time());
                    xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                }
                break;
            }
            case NetworkEvent::WifiConfigModeEnter:
                // WiFi config mode enter is handled by WifiBoard internally
                break;
            case NetworkEvent::WifiConfigModeExit:
                // WiFi config mode exit is handled by WifiBoard internally
                break;
            // Cellular modem specific events
            case NetworkEvent::ModemDetecting:
                display->SetStatus(Lang::Strings::DETECTING_MODULE);
                break;
            case NetworkEvent::ModemErrorNoSim:
                Alert(Lang::Strings::ERROR, Lang::Strings::PIN_ERROR, "warning",
                      Lang::Sounds::OGG_ERR_PIN);
                break;
            case NetworkEvent::ModemErrorRegDenied:
                Alert(Lang::Strings::ERROR, Lang::Strings::REG_ERROR, "warning",
                      Lang::Sounds::OGG_ERR_REG);
                break;
            case NetworkEvent::ModemErrorInitFailed:
                Alert(Lang::Strings::ERROR, Lang::Strings::MODEM_INIT_ERROR, "warning",
                      Lang::Sounds::OGG_EXCLAMATION);
                break;
            case NetworkEvent::ModemErrorTimeout:
                display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                break;
        }
    });

    // Start network asynchronously
    board.StartNetwork();

    // Update the status bar immediately to show the network state
    display->UpdateStatusBar(true);
}

void Application::Run() {
    // Set the priority of the main task to 10
    vTaskPrioritySet(nullptr, 10);

    const EventBits_t ALL_EVENTS =
        MAIN_EVENT_SCHEDULE | MAIN_EVENT_SEND_AUDIO | MAIN_EVENT_WAKE_WORD_DETECTED |
        MAIN_EVENT_VAD_CHANGE | MAIN_EVENT_CLOCK_TICK | MAIN_EVENT_ERROR |
        MAIN_EVENT_NETWORK_CONNECTED | MAIN_EVENT_NETWORK_DISCONNECTED | MAIN_EVENT_TOGGLE_CHAT |
        MAIN_EVENT_START_LISTENING | MAIN_EVENT_STOP_LISTENING | MAIN_EVENT_ACTIVATION_DONE |
        MAIN_EVENT_STATE_CHANGED | MAIN_EVENT_PLAYBACK_DRAINED | MAIN_EVENT_BARGE_IN_DETECTED;

    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, ALL_EVENTS, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & MAIN_EVENT_ERROR) {
            if (schedule_alert_active_) {
                ESP_LOGE(TAG, "Reminder network error: %s", last_error_message_.c_str());
                if (reminder_delivery_.NeedsServerAbort()) {
                    reminder_delivery_.Reset();
                    schedule_reminder_tts_deadline_us_ = 0;
                    RestoreScheduleAlertVolumeAfterDelivery();
                }
                SetDeviceState(kDeviceStateIdle);
            } else {
                SetDeviceState(kDeviceStateIdle);
                Alert(Lang::Strings::ERROR, last_error_message_.c_str(), "cancel",
                      Lang::Sounds::OGG_EXCLAMATION);
            }
        }

        if (bits & MAIN_EVENT_NETWORK_CONNECTED) {
            HandleNetworkConnectedEvent();
        }

        if (bits & MAIN_EVENT_NETWORK_DISCONNECTED) {
            HandleNetworkDisconnectedEvent();
        }

        if (bits & MAIN_EVENT_ACTIVATION_DONE) {
            HandleActivationDoneEvent();
        }

        if (bits & MAIN_EVENT_STATE_CHANGED) {
            HandleStateChangedEvent();
        }

        if (bits & MAIN_EVENT_PLAYBACK_DRAINED) {
            if (!IsProactiveConnectionBusy() && schedule_alert_active_ &&
                audio_service_.IsPlaybackIdle() &&
                reminder_delivery_.OnPlaybackDrained()) {
                if (NotifyReminderTriggered(active_schedule_task_, std::time(nullptr))) {
                    const int timeout_seconds = active_schedule_task_.kind ==
                        schedule::Kind::kBriefing ? kBriefingTtsStartTimeoutSeconds :
                        kReminderTtsStartTimeoutSeconds;
                    schedule_reminder_tts_deadline_us_ = esp_timer_get_time() +
                        timeout_seconds * 1000000LL;
                } else {
                    reminder_delivery_.CancelWaitingForTts();
                    RestoreScheduleAlertVolumeAfterDelivery();
                }
            }

            // Deferred listening start (auto mode): the playback queue has
            // drained, so it is now safe to enable voice processing.
            if (pending_listening_start_ && GetDeviceState() == kDeviceStateListening &&
                audio_service_.IsPlaybackIdle()) {
                pending_listening_start_ = false;
                StartListeningAudio();
            }
            if (!IsProactiveConnectionBusy() && pending_proactive_event_ &&
                audio_service_.IsPlaybackIdle()) {
                auto event = std::move(*pending_proactive_event_);
                pending_proactive_event_.reset();
                const bool sent = SendProactiveEvent(event);
                RecordProactiveSendResult(sent);
                if (sent) {
                    proactive_manager_.RecordDelivered(
                        event, std::time(nullptr), has_server_time_.load());
                } else {
                    event.metadata["cue_played"] = "true";
                    try {
                        proactive_queue_.Push(event);
                    } catch (const std::exception& error) {
                        ESP_LOGE(TAG, "Cannot requeue proactive follow-up: %s", error.what());
                        pending_proactive_event_ = std::move(event);
                    }
                }
                TrySaveProactive("playback drained");
            }
        }

        if (bits & MAIN_EVENT_TOGGLE_CHAT) {
            HandleToggleChatEvent();
        }

        if (bits & MAIN_EVENT_START_LISTENING) {
            HandleStartListeningEvent();
        }

        if (bits & MAIN_EVENT_STOP_LISTENING) {
            HandleStopListeningEvent();
        }

        if (bits & MAIN_EVENT_SEND_AUDIO) {
            if (IsProactiveConnectionBusy()) {
                proactive_deferred_event_bits_ |= MAIN_EVENT_SEND_AUDIO;
            } else {
                while (auto packet = audio_service_.PopPacketFromSendQueue()) {
                    if (protocol_ && !protocol_->SendAudio(std::move(packet))) {
                        // Drop the remaining packets. Leaving them in the queue would
                        // stall the Opus codec task (it waits for queue space), which in
                        // turn deadlocks the whole audio input pipeline, as no new
                        // MAIN_EVENT_SEND_AUDIO event would ever be triggered again.
                        while (audio_service_.PopPacketFromSendQueue())
                            ;
                        break;
                    }
                }
            }
        }

        if (bits & MAIN_EVENT_WAKE_WORD_DETECTED) {
            HandleWakeWordDetectedEvent();
        }

        if (bits & MAIN_EVENT_VAD_CHANGE) {
            if (GetDeviceState() == kDeviceStateListening) {
                auto led = Board::GetInstance().GetLed();
                led->OnStateChanged();
            }
        }

        if (bits & MAIN_EVENT_BARGE_IN_DETECTED) {
            if (GetDeviceState() == kDeviceStateSpeaking && !aborted_) {
                ESP_LOGI(TAG, "Barge-in confirmed, stopping playback locally");
                barge_in_detection_active_.store(false);
                audio_service_.EnableBargeInDetection(false);
                AbortSpeaking(kAbortReasonNone);
                audio_service_.ResetDecoder();
                SetListeningMode(GetDefaultListeningMode());
            }
        }

        if (bits & MAIN_EVENT_SCHEDULE) {
            std::unique_lock<std::mutex> lock(mutex_);
            auto tasks = std::move(main_tasks_);
            lock.unlock();
            for (auto& task : tasks) {
                task();
            }
        }

        if (bits & MAIN_EVENT_CLOCK_TICK) {
            clock_ticks_++;
            uptime_ticks_++;
            CheckSchedules();
            CheckProactiveEvents();
            auto display = Board::GetInstance().GetDisplay();
            display->UpdateStatusBar();

            // Print debug info every 10 seconds
            if (clock_ticks_ % 10 == 0) {
                SystemInfo::PrintHeapStats();
                // SystemInfo::PrintTaskList();
                // SystemInfo::PrintTaskCpuUsage(pdMS_TO_TICKS(1000));
            }
        }
    }
}

void Application::HandleNetworkConnectedEvent() {
    ESP_LOGI(TAG, "Network connected");
    auto state = GetDeviceState();

    if (state == kDeviceStateStarting || state == kDeviceStateWifiConfiguring) {
        // Network is ready, start activation
        SetDeviceState(kDeviceStateActivating);
        if (activation_task_handle_ != nullptr) {
            ESP_LOGW(TAG, "Activation task already running");
            return;
        }

        xTaskCreate(
            [](void* arg) {
                Application* app = static_cast<Application*>(arg);
                app->ActivationTask();
                app->activation_task_handle_ = nullptr;
                vTaskDelete(NULL);
            },
            "activation", 4096 * 2, this, 2, &activation_task_handle_);
    }

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleNetworkDisconnectedEvent() {
    if (has_server_time_.load()) {
        const auto now = std::time(nullptr);
        const auto now_us = esp_timer_get_time();
        size_t disconnect_count = 0;
        {
            std::lock_guard<std::mutex> lock(network_health_mutex_);
            while (!network_disconnect_us_.empty() &&
                   now_us - network_disconnect_us_.front() > 300LL * 1000000) {
                network_disconnect_us_.pop_front();
            }
            disconnect_count = network_disconnect_us_.size();
        }
        if (disconnect_count >= 3) {
            if (auto event = health_tracker_.Raise(
                    "network_flapping", proactive::Severity::kWarning, now,
                    {{"disconnects_in_5m", std::to_string(disconnect_count)}})) {
                QueueHealthEvent(*event);
            }
        }
    }
    // Close current conversation when network disconnected
    auto state = GetDeviceState();
    if (!IsProactiveConnectionBusy() &&
        (state == kDeviceStateConnecting || state == kDeviceStateListening ||
         state == kDeviceStateSpeaking)) {
        ESP_LOGI(TAG, "Closing audio channel due to network disconnection");
        protocol_->CloseAudioChannel();
    }

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleActivationDoneEvent() {
    ESP_LOGI(TAG, "Activation done");

    SystemInfo::PrintHeapStats();
    SetDeviceState(kDeviceStateIdle);

    has_server_time_ = ota_->HasServerTime();
    if (has_server_time_.load()) {
        if (auto recovered = health_tracker_.Recover("time_unsynchronized", std::time(nullptr))) {
            QueueHealthEvent(*recovered);
        }
    }

    auto display = Board::GetInstance().GetDisplay();
    std::string message = std::string(Lang::Strings::VERSION) + ota_->GetCurrentVersion();
    display->ShowNotification(message.c_str());
    display->SetChatMessage("system", "");

    // Release OTA object after activation is complete
    ota_.reset();
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);

    Schedule([this]() {
        // Play the success sound to indicate the device is ready
        audio_service_.PlaySound(Lang::Sounds::OGG_SUCCESS);
    });
}

void Application::ActivationTask() {
    // Create OTA object for activation process
    ota_ = std::make_unique<Ota>();

    // Check for new assets version
    CheckAssetsVersion();

    // Check for new firmware version
    CheckNewVersion();

    // Initialize the protocol
    InitializeProtocol();

    // Signal completion to main loop
    xEventGroupSetBits(event_group_, MAIN_EVENT_ACTIVATION_DONE);
}

void Application::CheckAssetsVersion() {
    // Only allow CheckAssetsVersion to be called once
    if (assets_version_checked_) {
        return;
    }
    assets_version_checked_ = true;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto& assets = Assets::GetInstance();

    if (!assets.partition_valid()) {
        ESP_LOGW(TAG, "Assets partition is disabled for board %s", BOARD_NAME);
        return;
    }

    Settings settings("assets", true);
    // Check if there is a new assets need to be downloaded
    std::string download_url = settings.GetString("download_url");

    if (!download_url.empty()) {
        settings.EraseKey("download_url");

        char message[256];
        snprintf(message, sizeof(message), Lang::Strings::FOUND_NEW_ASSETS, download_url.c_str());
        Alert(Lang::Strings::LOADING_ASSETS, message, "cloud_download", Lang::Sounds::OGG_UPGRADE);

        // Wait for the audio service to be idle for 3 seconds
        vTaskDelay(pdMS_TO_TICKS(3000));
        SetDeviceState(kDeviceStateUpgrading);
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        display->SetChatMessage("system", Lang::Strings::PLEASE_WAIT);

        bool success =
            assets.Download(download_url, [this, display](int progress, size_t speed) -> void {
                char buffer[32];
                snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
                Schedule([display, message = std::string(buffer)]() {
                    display->SetChatMessage("system", message.c_str());
                });
            });

        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!success) {
            Alert(Lang::Strings::ERROR, Lang::Strings::DOWNLOAD_ASSETS_FAILED, "cancel",
                  Lang::Sounds::OGG_EXCLAMATION);
            vTaskDelay(pdMS_TO_TICKS(2000));
            SetDeviceState(kDeviceStateActivating);
            return;
        }
    }

    // Apply assets
    assets.Apply();
    display->SetChatMessage("system", "");
    display->SetEmotion("robot_2");
}

void Application::CheckNewVersion() {
    const int MAX_RETRY = 10;
    int retry_count = 0;
    int retry_delay = 10;  // Initial retry delay in seconds

    auto& board = Board::GetInstance();
    while (true) {
        auto display = board.GetDisplay();
        display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);

        esp_err_t err = ota_->CheckVersion();
        if (err != ESP_OK) {
            retry_count++;
            if (retry_count >= MAX_RETRY) {
                ESP_LOGE(TAG, "Too many retries, exit version check");
                return;
            }

            char error_message[128];
            snprintf(error_message, sizeof(error_message), "code=%d, url=%s", err,
                     ota_->GetCheckVersionUrl().c_str());
            char buffer[256];
            snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED, retry_delay,
                     error_message);
            Alert(Lang::Strings::ERROR, buffer, "cloud_off", Lang::Sounds::OGG_EXCLAMATION);

            ESP_LOGW(TAG, "Check new version failed, retry in %d seconds (%d/%d)", retry_delay,
                     retry_count, MAX_RETRY);
            for (int i = 0; i < retry_delay; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (GetDeviceState() == kDeviceStateIdle) {
                    break;
                }
            }
            retry_delay *= 2;  // Double the retry delay
            continue;
        }
        retry_count = 0;
        retry_delay = 10;  // Reset retry delay

        if (ota_->HasNewVersion()) {
            const std::string firmware_version = ota_->GetFirmwareVersion();
            Schedule([this, firmware_version]() {
                if (auto event = health_tracker_.Raise(
                        "ota_update_available", proactive::Severity::kInfo,
                        std::time(nullptr), {{"version", firmware_version}})) {
                    QueueHealthEvent(*event);
                }
            });
            if (UpgradeFirmware(ota_->GetFirmwareUrl(), ota_->GetFirmwareVersion())) {
                return;  // This line will never be reached after reboot
            }
            // If upgrade failed, continue to normal operation
        }

        // No new version, mark the current version as valid
        ota_->MarkCurrentVersionValid();
        if (!ota_->HasActivationCode() && !ota_->HasActivationChallenge()) {
            // Exit the loop if done checking new version
            break;
        }

        display->SetStatus(Lang::Strings::ACTIVATION);
        // Activation code is shown to the user and waiting for the user to input
        if (ota_->HasActivationCode()) {
            ShowActivationCode(ota_->GetActivationCode(), ota_->GetActivationMessage());
        }

        // This will block the loop until the activation is done or timeout
        for (int i = 0; i < 10; ++i) {
            ESP_LOGI(TAG, "Activating... %d/%d", i + 1, 10);
            esp_err_t err = ota_->Activate();
            if (err == ESP_OK) {
                break;
            } else if (err == ESP_ERR_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(3000));
            } else {
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
            if (GetDeviceState() == kDeviceStateIdle) {
                break;
            }
        }
    }
}

void Application::InitializeProtocol() {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto codec = board.GetAudioCodec();

    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

    if (ota_->HasMqttConfig()) {
        protocol_ = std::make_unique<MqttProtocol>();
    } else if (ota_->HasWebsocketConfig()) {
        protocol_ = std::make_unique<WebsocketProtocol>();
    } else {
        ESP_LOGW(TAG, "No protocol specified in the OTA config, using MQTT");
        protocol_ = std::make_unique<MqttProtocol>();
    }

    protocol_->OnConnected([this]() { DismissAlert(); });

    protocol_->OnNetworkError([this](const std::string& message) {
        last_error_message_ = message;
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
    });

    protocol_->OnIncomingAudio([this](std::unique_ptr<AudioStreamPacket> packet) {
        if (GetDeviceState() == kDeviceStateSpeaking && !aborted_) {
            packet->lyrics_generation = netease_lyrics_.Generation();
            if (!barge_in_detection_active_.exchange(true)) {
                audio_service_.EnableBargeInDetection(true);
            }
            audio_service_.PushPacketToDecodeQueue(std::move(packet));
        }
    });

    protocol_->OnAudioChannelOpened([this, codec, &board]() {
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG,
                     "Server sample rate %d does not match device output sample rate %d, "
                     "resampling may cause distortion",
                     protocol_->server_sample_rate(), codec->output_sample_rate());
        }
    });

    protocol_->OnAudioChannelClosed([this, &board]() {
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        netease_lyrics_.Clear();
        Schedule([this]() {
            if (reminder_delivery_.NeedsServerAbort()) {
                reminder_delivery_.Reset();
                schedule_reminder_tts_deadline_us_ = 0;
                RestoreScheduleAlertVolumeAfterDelivery();
            }
            auto display = Board::GetInstance().GetDisplay();
            display->CloseNeteaseMusicLyrics();
            if (!schedule_alert_active_) {
                display->SetChatMessage("system", "");
            }
            SetDeviceState(kDeviceStateIdle);
        });
    });

    protocol_->OnIncomingJson([this, display](const cJSON* root) {
        // Parse JSON data
        auto type = cJSON_GetObjectItem(root, "type");
        if (!cJSON_IsString(type)) {
            ESP_LOGW(TAG, "Incoming JSON message has no type");
            return;
        }
        if (strcmp(type->valuestring, "tts") == 0) {
            auto state = cJSON_GetObjectItem(root, "state");
            if (!cJSON_IsString(state)) {
                return;
            }
            if (strcmp(state->valuestring, "start") == 0) {
                Schedule([this]() {
                    if (schedule_alert_active_) {
                        if (!reminder_delivery_.OnTtsStarted()) {
                            ESP_LOGW(TAG, "Ignoring unexpected TTS start for schedule alert");
                            return;
                        }
                        schedule_reminder_tts_deadline_us_ = 0;
                    }
                    aborted_ = false;
                    SetDeviceState(kDeviceStateSpeaking);
                });
            } else if (strcmp(state->valuestring, "stop") == 0) {
                Schedule([this]() {
                    if (reminder_delivery_.OnTtsStopped()) {
                        schedule_reminder_tts_deadline_us_ = 0;
                        RestoreScheduleAlertVolumeAfterDelivery();
                        if (schedule_alert_active_ &&
                            active_schedule_task_.kind == schedule::Kind::kAlarm) {
                            ShowScheduleAlertPage();
                        } else if (schedule_alert_active_) {
                            if (active_schedule_task_.kind == schedule::Kind::kReminder) {
                                const auto now = std::time(nullptr);
                                proactive::Event follow_up{
                                    "follow-up-policy-" + std::to_string(active_schedule_task_.id),
                                    "follow_up", proactive::Priority::kNormal,
                                    "confirm reminder completion", now, now + 900,
                                    "follow-up:" + std::to_string(active_schedule_task_.id), true, {}};
                                if (proactive_manager_.ShouldDeliver(
                                        follow_up, now, has_server_time_.load())) {
                                    try {
                                        schedule_follow_ups_.Schedule(
                                            active_schedule_task_.id,
                                            active_schedule_task_.label,
                                            active_schedule_task_.trigger_at, now);
                                        TrySaveProactive("reminder follow-up");
                                    } catch (const std::exception& error) {
                                        ESP_LOGE(TAG, "Cannot persist reminder follow-up: %s",
                                                 error.what());
                                        Board::GetInstance().GetDisplay()->ShowNotification(
                                            "提醒完成追踪存储已满", 5000);
                                    }
                                }
                            }
                            FinishScheduleAlert();
                        }
                        listening_mode_ = GetDefaultListeningMode();
                        SetDeviceState(kDeviceStateListening);
                        if (!schedule_alert_active_) {
                            StartNextScheduleAlert();
                        }
                        return;
                    }
                    if (GetDeviceState() == kDeviceStateSpeaking) {
                        if (listening_mode_ == kListeningModeManualStop) {
                            SetDeviceState(kDeviceStateIdle);
                        } else {
                            SetDeviceState(kDeviceStateListening);
                        }
                    }
                });
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                auto text = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(text)) {
                    std::vector<TextGlyph> glyphs;
                    uint8_t bpp = 0;
                    if (!TextGlyphPayload::Parse(root, glyphs, bpp)) {
                        glyphs.clear();
                    }
                    ESP_LOGI(TAG, "<< %s", text->valuestring);
                    Schedule([display, message = LocalizeToolStatusMessage(text->valuestring),
                              glyphs = std::move(glyphs), bpp]() {
                        display->AddTextGlyphs(glyphs, bpp);
                        display->SetChatMessage("assistant", message.c_str());
                    });
                }
            }
        } else if (strcmp(type->valuestring, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text)) {
                std::vector<TextGlyph> glyphs;
                uint8_t bpp = 0;
                if (!TextGlyphPayload::Parse(root, glyphs, bpp)) {
                    glyphs.clear();
                }
                ESP_LOGI(TAG, ">> %s", text->valuestring);
                Schedule([display, message = LocalizeToolStatusMessage(text->valuestring),
                          glyphs = std::move(glyphs), bpp]() {
                    display->AddTextGlyphs(glyphs, bpp);
                    display->SetChatMessage("user", message.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "llm") == 0) {
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(emotion)) {
                Schedule([display, emotion_str = std::string(emotion->valuestring)]() {
                    display->SetEmotion(emotion_str.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "mcp") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            if (cJSON_IsObject(payload)) {
                if (!HandleNeteaseLyricsNotification(payload)) {
                    McpServer::GetInstance().ParseMessage(payload);
                }
            }
        } else if (strcmp(type->valuestring, "system") == 0) {
            auto command = cJSON_GetObjectItem(root, "command");
            if (cJSON_IsString(command)) {
                ESP_LOGI(TAG, "System command: %s", command->valuestring);
                if (strcmp(command->valuestring, "reboot") == 0) {
                    // Do a reboot if user requests a OTA update
                    Schedule([this]() { Reboot(); });
                } else {
                    ESP_LOGW(TAG, "Unknown system command: %s", command->valuestring);
                }
            }
        } else if (strcmp(type->valuestring, "alert") == 0) {
            auto status = cJSON_GetObjectItem(root, "status");
            auto message = cJSON_GetObjectItem(root, "message");
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(status) && cJSON_IsString(message) && cJSON_IsString(emotion)) {
                Alert(status->valuestring, message->valuestring, emotion->valuestring,
                      Lang::Sounds::OGG_VIBRATION);
            } else {
                ESP_LOGW(TAG, "Alert command requires status, message and emotion");
            }
#if CONFIG_RECEIVE_CUSTOM_MESSAGE
        } else if (strcmp(type->valuestring, "custom") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            ESP_LOGI(TAG, "Received custom message: %s", cJSON_PrintUnformatted(root));
            if (cJSON_IsObject(payload)) {
                Schedule(
                    [this, display, payload_str = std::string(cJSON_PrintUnformatted(payload))]() {
                        display->SetChatMessage("system", payload_str.c_str());
                    });
            } else {
                ESP_LOGW(TAG, "Invalid custom message format: missing payload");
            }
#endif
        } else {
            ESP_LOGW(TAG, "Unknown message type: %s", type->valuestring);
        }
    });

    protocol_->Start();
}

bool Application::HandleNeteaseLyricsNotification(const cJSON* payload) {
    auto jsonrpc = cJSON_GetObjectItem(payload, "jsonrpc");
    auto method = cJSON_GetObjectItem(payload, "method");
    if (!cJSON_IsString(method) ||
        strcmp(method->valuestring, "notifications/netease_music/lyrics") != 0) {
        return false;
    }
    if (!cJSON_IsString(jsonrpc) || strcmp(jsonrpc->valuestring, "2.0") != 0) {
        ESP_LOGW(TAG, "Ignoring malformed NetEase lyrics notification");
        return true;
    }
    auto params = cJSON_GetObjectItem(payload, "params");
    auto version = cJSON_GetObjectItem(params, "version");
    auto action = cJSON_GetObjectItem(params, "action");
    if (!cJSON_IsObject(params) || !cJSON_IsNumber(version) || version->valueint != 1 ||
        !cJSON_IsString(action)) {
        ESP_LOGW(TAG, "Ignoring malformed NetEase lyrics notification");
        return true;
    }
    if (strcmp(action->valuestring, "clear") == 0) {
        auto reason = cJSON_GetObjectItem(params, "reason");
        if (!cJSON_IsString(reason) ||
            (strcmp(reason->valuestring, "paused") != 0 &&
             strcmp(reason->valuestring, "stopped") != 0 &&
             strcmp(reason->valuestring, "interrupted") != 0 &&
             strcmp(reason->valuestring, "completed") != 0 &&
             strcmp(reason->valuestring, "closed") != 0)) {
            ESP_LOGW(TAG, "Ignoring invalid NetEase lyrics clear notification");
            return true;
        }
        netease_lyrics_.Clear();
        Schedule([]() { Board::GetInstance().GetDisplay()->CloseNeteaseMusicLyrics(); });
        ESP_LOGI(TAG, "NetEase lyrics cleared");
        return true;
    }
    if (strcmp(action->valuestring, "start") != 0) {
        ESP_LOGW(TAG, "Ignoring unknown NetEase lyrics action");
        return true;
    }

    netease_music::LyricsPlayback playback;
    if (!ParseLyricsStart(params, playback)) {
        ESP_LOGW(TAG, "Ignoring invalid NetEase lyrics start notification");
        return true;
    }
    const auto initial = [&playback]() {
        netease_music::LyricsWindow window;
        if (!playback.available) {
            window.current = "暂无歌词";
        } else if (!playback.lines.empty()) {
            window.next = playback.lines.front().text;
        }
        return window;
    }();
    const auto playback_id = playback.playback_id;
    const auto track_id = playback.track_id;
    const size_t line_count = playback.lines.size();
    auto title = playback.title;
    auto artists = playback.artists;
    netease_lyrics_.Start(std::move(playback));
    Schedule([title = std::move(title), artists = std::move(artists), initial]() {
        auto display = Board::GetInstance().GetDisplay();
        display->ShowNeteaseMusicLyrics(title, artists, initial.previous, initial.current,
                                        initial.next);
    });
    ESP_LOGI(TAG, "NetEase lyrics started: playback_id=%s track_id=%s lines=%u",
             playback_id.c_str(), track_id.c_str(), static_cast<unsigned>(line_count));
    return true;
}

void Application::ShowActivationCode(const std::string& code, const std::string& message) {
    struct digit_sound {
        char digit;
        const std::string_view& sound;
    };
    static const std::array<digit_sound, 10> digit_sounds{
        {digit_sound{'0', Lang::Sounds::OGG_0}, digit_sound{'1', Lang::Sounds::OGG_1},
         digit_sound{'2', Lang::Sounds::OGG_2}, digit_sound{'3', Lang::Sounds::OGG_3},
         digit_sound{'4', Lang::Sounds::OGG_4}, digit_sound{'5', Lang::Sounds::OGG_5},
         digit_sound{'6', Lang::Sounds::OGG_6}, digit_sound{'7', Lang::Sounds::OGG_7},
         digit_sound{'8', Lang::Sounds::OGG_8}, digit_sound{'9', Lang::Sounds::OGG_9}}};

    // This sentence uses 9KB of SRAM, so we need to wait for it to finish
    Alert(Lang::Strings::ACTIVATION, message.c_str(), "link", Lang::Sounds::OGG_ACTIVATION);

    for (const auto& digit : code) {
        auto it = std::find_if(digit_sounds.begin(), digit_sounds.end(),
                               [digit](const digit_sound& ds) { return ds.digit == digit; });
        if (it != digit_sounds.end()) {
            audio_service_.PlaySound(it->sound);
        }
    }
}

void Application::Alert(const char* status, const char* message, const char* emotion,
                        const std::string_view& sound) {
    ESP_LOGW(TAG, "Alert [%s] %s: %s", emotion, status, message);
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    display->SetEmotion(emotion);
    display->SetChatMessage("system", message);
    if (!sound.empty()) {
        audio_service_.PlaySound(sound);
    }
}

void Application::DismissAlert() {
    if (GetDeviceState() == kDeviceStateIdle) {
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::STANDBY);
        display->SetEmotion("neutral");
        display->SetChatMessage("system", "");
    }
}

void Application::ToggleChatState() { xEventGroupSetBits(event_group_, MAIN_EVENT_TOGGLE_CHAT); }

void Application::StartListening() { xEventGroupSetBits(event_group_, MAIN_EVENT_START_LISTENING); }

void Application::StopListening() { xEventGroupSetBits(event_group_, MAIN_EVENT_STOP_LISTENING); }

void Application::HandleToggleChatEvent() {
    if (IsProactiveConnectionBusy()) {
        proactive_deferred_event_bits_ |= MAIN_EVENT_TOGGLE_CHAT;
        return;
    }
    auto state = GetDeviceState();

    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    } else if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (state == kDeviceStateIdle) {
        ListeningMode mode = GetDefaultListeningMode();
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this, mode]() { ContinueOpenAudioChannel(mode); });
            return;
        }
        SetListeningMode(mode);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
    } else if (state == kDeviceStateListening) {
        protocol_->CloseAudioChannel();
    }
}

void Application::ContinueOpenAudioChannel(ListeningMode mode) {
    // Check state again in case it was changed during scheduling
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }
    if (IsProactiveConnectionBusy()) {
        proactive_deferred_open_mode_ = mode;
        return;
    }

    // Switch to performance mode before connecting to reduce latency
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

    if (!protocol_->IsAudioChannelOpened()) {
        if (!protocol_->OpenAudioChannel()) {
            // Return to idle so the device is not stuck in the connecting
            // state (not every failure path reports a network error)
            SetDeviceState(kDeviceStateIdle);
            return;
        }
    }

    SetListeningMode(mode);
}

void Application::HandleStartListeningEvent() {
    if (IsProactiveConnectionBusy()) {
        proactive_deferred_event_bits_ |= MAIN_EVENT_START_LISTENING;
        return;
    }
    auto state = GetDeviceState();

    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (state == kDeviceStateIdle) {
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this]() { ContinueOpenAudioChannel(kListeningModeManualStop); });
            return;
        }
        SetListeningMode(kListeningModeManualStop);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
        SetListeningMode(kListeningModeManualStop);
    }
}

void Application::HandleStopListeningEvent() {
    if (IsProactiveConnectionBusy()) {
        proactive_deferred_event_bits_ |= MAIN_EVENT_STOP_LISTENING;
        return;
    }
    auto state = GetDeviceState();

    if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    } else if (state == kDeviceStateListening) {
        if (protocol_) {
            protocol_->SendStopListening();
        }
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::HandleWakeWordDetectedEvent() {
    if (IsProactiveConnectionBusy()) {
        proactive_deferred_event_bits_ |= MAIN_EVENT_WAKE_WORD_DETECTED;
        return;
    }
    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();
    auto wake_word = audio_service_.GetLastWakeWord();
    ESP_LOGI(TAG, "Wake word detected: %s (state: %d)", wake_word.c_str(), (int)state);

    if (state == kDeviceStateIdle) {
        BeginWakeWordInvoke(wake_word);
    } else if (state == kDeviceStateSpeaking || state == kDeviceStateListening) {
        AbortSpeaking(kAbortReasonWakeWordDetected);
        // Clear send queue to avoid sending residues to server
        while (audio_service_.PopPacketFromSendQueue())
            ;

        if (state == kDeviceStateListening) {
            protocol_->SendStartListening(GetDefaultListeningMode());
            audio_service_.ResetDecoder();
            audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
            // Re-enable wake word detection as it was stopped by the detection itself
            audio_service_.EnableWakeWordDetection(true);
        } else {
            // Play popup sound and start listening again
            play_popup_on_listening_ = true;
            SetListeningMode(GetDefaultListeningMode());
        }
    } else if (state == kDeviceStateActivating) {
        // Restart the activation check if the wake word is detected during activation
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::BeginWakeWordInvoke(const std::string& wake_word) {
    if (IsProactiveConnectionBusy()) {
        proactive_deferred_wake_word_ = wake_word;
        return;
    }
    // Must run in the main task with the device in idle state
    audio_service_.EncodeWakeWord();

    // Always pass through the connecting state, even if the audio channel is
    // already opened. ContinueWakeWordInvoke() rejects any other state, so
    // skipping this transition would silently drop the wake word invocation.
    if (!SetDeviceState(kDeviceStateConnecting)) {
        // Wake word detection was stopped by the detection itself; restore it
        // so the device does not become unresponsive to wake words.
        audio_service_.EnableWakeWordDetection(true);
        return;
    }

    if (!protocol_->IsAudioChannelOpened()) {
        // Schedule to let the state change be processed first (UI update),
        // then continue with OpenAudioChannel which may block for ~1 second
        Schedule([this, wake_word]() { ContinueWakeWordInvoke(wake_word); });
        return;
    }
    // Channel already opened, continue directly
    ContinueWakeWordInvoke(wake_word);
}

void Application::ContinueWakeWordInvoke(const std::string& wake_word) {
    // Check state again in case it was changed during scheduling
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }
    if (IsProactiveConnectionBusy()) {
        proactive_deferred_wake_word_ = wake_word;
        return;
    }

    // Switch to performance mode before connecting to reduce latency
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

    if (!protocol_->IsAudioChannelOpened()) {
        if (!protocol_->OpenAudioChannel()) {
            // Return to idle so the device is not stuck in the connecting
            // state (not every failure path reports a network error), and
            // wake word detection is re-enabled by the idle state handler.
            SetDeviceState(kDeviceStateIdle);
            return;
        }
    }

    ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_SEND_WAKE_WORD_DATA
    // Encode and send the wake word data to the server
    while (auto packet = audio_service_.PopWakeWordPacket()) {
        protocol_->SendAudio(std::move(packet));
    }
    // Set the chat state to wake word detected
    protocol_->SendWakeWordDetected(wake_word);
    SetListeningMode(GetDefaultListeningMode());
#else
    // Set flag to play popup sound after state changes to listening
    // (PlaySound here would be cleared by ResetDecoder in EnableVoiceProcessing)
    play_popup_on_listening_ = true;
    SetListeningMode(GetDefaultListeningMode());
#endif
}

void Application::HandleStateChangedEvent() {
    DeviceState new_state = state_machine_.GetState();
    clock_ticks_ = 0;
    // Any state change invalidates a pending deferred listening start;
    // the Listening case below re-arms it when needed.
    pending_listening_start_ = false;
    if (new_state != kDeviceStateSpeaking) {
        barge_in_detection_active_.store(false);
        audio_service_.EnableBargeInDetection(false);
    }

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto led = board.GetLed();
    led->OnStateChanged();

    switch (new_state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
            display->SetStatus(Lang::Strings::STANDBY);
            if (!schedule_alert_active_) {
                display->ClearChatMessages();  // Clear messages first
            }
            display->SetEmotion("neutral");  // Then set emotion (wechat mode checks child count)
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(!schedule_alert_active_);
            break;
        case kDeviceStateConnecting:
            display->SetStatus(Lang::Strings::CONNECTING);
            display->SetEmotion("neutral");
            display->SetChatMessage("system", "");
            break;
        case kDeviceStateListening:
            display->SetStatus(Lang::Strings::LISTENING);
            display->SetEmotion("neutral");

            // Make sure the audio processor is running
            if (play_popup_on_listening_ || !audio_service_.IsAudioProcessorRunning()) {
                // For auto mode, wait for the playback queue to drain before enabling
                // voice processing. This prevents audio truncation when STOP arrives
                // late due to network jitter. Instead of blocking the main loop here,
                // defer the start until MAIN_EVENT_PLAYBACK_DRAINED arrives.
                if (listening_mode_ == kListeningModeAutoStop && !audio_service_.IsPlaybackIdle()) {
                    pending_listening_start_ = true;
                } else {
                    StartListeningAudio();
                }
            } else {
                ConfigureWakeWordForListening();
            }
            break;
        case kDeviceStateSpeaking:
            display->SetStatus(Lang::Strings::SPEAKING);

            if (listening_mode_ != kListeningModeRealtime) {
                audio_service_.EnableVoiceProcessing(false);
                // Only AFE wake word can be detected in speaking mode
                audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
            }
            audio_service_.ResetDecoder();
            barge_in_detection_active_.store(false);
            audio_service_.EnableBargeInDetection(false);
            break;
        case kDeviceStateWifiConfiguring:
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(false);
            break;
        default:
            // Do nothing
            break;
    }
}

void Application::StartListeningAudio() {
    // Runs in the main loop, either directly from HandleStateChangedEvent or
    // deferred via MAIN_EVENT_PLAYBACK_DRAINED once the playback queue drains.
    if (GetDeviceState() != kDeviceStateListening) {
        return;
    }
    if (IsProactiveConnectionBusy()) {
        proactive_deferred_event_bits_ |= MAIN_EVENT_STATE_CHANGED;
        return;
    }

    // Send the start listening command
    protocol_->SendStartListening(listening_mode_);
    audio_service_.EnableVoiceProcessing(true);

    ConfigureWakeWordForListening();

    // Play popup sound after ResetDecoder (in EnableVoiceProcessing) has been called
    if (play_popup_on_listening_) {
        play_popup_on_listening_ = false;
        audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
    }
}

void Application::ConfigureWakeWordForListening() {
#ifdef CONFIG_WAKE_WORD_DETECTION_IN_LISTENING
    // Enable wake word detection in listening mode (configured via Kconfig)
    audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
#else
    // Disable wake word detection in listening mode
    audio_service_.EnableWakeWordDetection(false);
#endif
}

void Application::Schedule(std::function<void()>&& callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

void Application::AbortSpeaking(AbortReason reason) {
    if (IsProactiveConnectionBusy()) {
        ESP_LOGW(TAG, "Deferring abort while proactive connection worker owns protocol");
        proactive_deferred_event_bits_ |= MAIN_EVENT_TOGGLE_CHAT;
        return;
    }
    ESP_LOGI(TAG, "Abort speaking");
    aborted_ = true;
    if (protocol_) {
        protocol_->SendAbortSpeaking(reason);
    }
}

void Application::SetListeningMode(ListeningMode mode) {
    listening_mode_ = mode;
    SetDeviceState(kDeviceStateListening);
}

ListeningMode Application::GetDefaultListeningMode() const {
    return aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime;
}

void Application::Reboot() {
    if (IsProactiveConnectionBusy()) {
        proactive_reboot_pending_ = true;
        return;
    }
    ESP_LOGI(TAG, "Rebooting...");
    // Disconnect the audio channel
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }
    protocol_.reset();
    audio_service_.Stop();

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

bool Application::UpgradeFirmware(const std::string& url, const std::string& version) {
    if (IsProactiveConnectionBusy()) {
        ESP_LOGE(TAG, "Cannot start firmware upgrade while proactive channel is connecting");
        return false;
    }
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();

    std::string upgrade_url = url;
    std::string version_info = version.empty() ? "(Manual upgrade)" : version;

    // Close audio channel if it's open
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        ESP_LOGI(TAG, "Closing audio channel before firmware upgrade");
        protocol_->CloseAudioChannel();
    }
    ESP_LOGI(TAG, "Starting firmware upgrade from URL: %s", upgrade_url.c_str());

    Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "download",
          Lang::Sounds::OGG_UPGRADE);
    vTaskDelay(pdMS_TO_TICKS(3000));

    SetDeviceState(kDeviceStateUpgrading);

    std::string message = std::string(Lang::Strings::NEW_VERSION) + version_info;
    display->SetChatMessage("system", message.c_str());

    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    audio_service_.Stop();
    vTaskDelay(pdMS_TO_TICKS(1000));

    bool upgrade_success = Ota::Upgrade(upgrade_url, [this, display](int progress, size_t speed) {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
        Schedule([display, message = std::string(buffer)]() {
            display->SetChatMessage("system", message.c_str());
        });
    });

    if (!upgrade_success) {
        // Upgrade failed, restart audio service and continue running
        ESP_LOGE(TAG,
                 "Firmware upgrade failed, restarting audio service and continuing operation...");
        audio_service_.Start();                              // Restart audio service
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);  // Restore power save level
        Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "cancel",
              Lang::Sounds::OGG_EXCLAMATION);
        vTaskDelay(pdMS_TO_TICKS(3000));
        return false;
    } else {
        // Upgrade success, reboot immediately
        ESP_LOGI(TAG, "Firmware upgrade successful, rebooting...");
        display->SetChatMessage("system", "Upgrade successful, rebooting...");
        vTaskDelay(pdMS_TO_TICKS(1000));  // Brief pause to show message
        Reboot();
        return true;
    }
}

void Application::WakeWordInvoke(const std::string& wake_word) {
    // This API may be called by a board callback task. All shared proactive and
    // protocol state is therefore inspected and changed only in the main task.
    Schedule([this, wake_word]() {
        if (IsProactiveConnectionBusy()) {
            proactive_deferred_wake_word_ = wake_word;
            return;
        }
        if (!protocol_) return;
        const auto state = GetDeviceState();
        if (state == kDeviceStateIdle) {
            BeginWakeWordInvoke(wake_word);
        } else if (state == kDeviceStateSpeaking) {
            AbortSpeaking(kAbortReasonNone);
        } else if (state == kDeviceStateListening) {
            protocol_->CloseAudioChannel();
        }
    });
}

bool Application::CanEnterSleepMode() {
    if (IsProactiveConnectionBusy()) {
        return false;
    }
    if (GetDeviceState() != kDeviceStateIdle) {
        return false;
    }

    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        return false;
    }

    if (!audio_service_.IsIdle()) {
        return false;
    }

    // Now it is safe to enter sleep mode
    return true;
}

void Application::RegisterMcpBroadcastCallback(std::function<void(const std::string&)> callback) {
    mcp_broadcast_callback_ = std::move(callback);
}

void Application::SendMcpMessage(const std::string& payload) {
    // Always schedule to run in main task for thread safety
    Schedule([this, payload]() {
        if (IsProactiveConnectionBusy()) {
            if (proactive_deferred_mcp_messages_.size() >= 8) {
                ESP_LOGE(TAG, "Deferred MCP queue is full while proactive channel connects");
            } else {
                proactive_deferred_mcp_messages_.push_back(payload);
            }
        } else if (protocol_) {
            protocol_->SendMcpMessage(payload);
        }
        if (mcp_broadcast_callback_) {
            mcp_broadcast_callback_(payload);
        }
    });
}

void Application::SetAecMode(AecMode mode) {
    aec_mode_ = mode;
    Schedule([this]() {
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        switch (aec_mode_) {
            case kAecOff:
                audio_service_.EnableDeviceAec(false);
                display->ShowNotification(Lang::Strings::RTC_MODE_OFF);
                break;
            case kAecOnServerSide:
                audio_service_.EnableDeviceAec(false);
                display->ShowNotification(Lang::Strings::RTC_MODE_ON);
                break;
            case kAecOnDeviceSide:
                audio_service_.EnableDeviceAec(true);
                display->ShowNotification(Lang::Strings::RTC_MODE_ON);
                break;
        }

        // If the AEC mode is changed, close the audio channel
        if (IsProactiveConnectionBusy()) {
            proactive_close_pending_ = true;
        } else if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
    });
}

void Application::PlaySound(const std::string_view& sound) { audio_service_.PlaySound(sound); }

void Application::LoadSchedules() {
    Settings settings("schedule");
    std::string saved;
    const int chunk_count = settings.GetInt("chunks", 0);
    if (chunk_count < 0 || chunk_count > 8) {
        throw std::runtime_error("NVS中的定时任务分片数无效");
    }
    for (int i = 0; i < chunk_count; ++i) {
        const std::string chunk = settings.GetString("data" + std::to_string(i));
        if (chunk.empty()) throw std::runtime_error("NVS中的定时任务分片缺失");
        saved += chunk;
    }
    if (saved.empty()) saved = settings.GetString("tasks");
    if (saved.empty()) {
        schedule_manager_.Restore({}, 1);
        return;
    }
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(cJSON_Parse(saved.c_str()), cJSON_Delete);
    if (!cJSON_IsObject(root.get())) throw std::runtime_error("NVS中的定时任务JSON损坏");
    cJSON* next_id = cJSON_GetObjectItem(root.get(), "next_id");
    cJSON* tasks = cJSON_GetObjectItem(root.get(), "tasks");
    if (!cJSON_IsNumber(next_id) || next_id->valuedouble < 1 || !cJSON_IsArray(tasks)) {
        throw std::runtime_error("NVS中的定时任务字段无效");
    }
    std::vector<schedule::Task> restored;
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, tasks) {
        cJSON* id = cJSON_GetObjectItem(item, "id");
        cJSON* kind = cJSON_GetObjectItem(item, "kind");
        cJSON* repeat = cJSON_GetObjectItem(item, "repeat");
        cJSON* label = cJSON_GetObjectItem(item, "label");
        cJSON* trigger_at = cJSON_GetObjectItem(item, "trigger_at");
        cJSON* weekdays = cJSON_GetObjectItem(item, "weekdays");
        cJSON* sections = cJSON_GetObjectItem(item, "sections");
        cJSON* location = cJSON_GetObjectItem(item, "location");
        if (!cJSON_IsNumber(id) || id->valuedouble < 1 || !cJSON_IsString(kind) ||
            !cJSON_IsString(repeat) || !cJSON_IsString(label) || !cJSON_IsNumber(trigger_at)) {
            throw std::runtime_error("NVS中的定时任务条目无效");
        }
        schedule::Task task;
        task.id = static_cast<uint32_t>(id->valuedouble);
        task.kind = schedule::Manager::ParseKind(kind->valuestring);
        task.repeat = schedule::Manager::ParseRepeat(repeat->valuestring);
        task.label = label->valuestring;
        if (task.kind == schedule::Kind::kBriefing) {
            if (!cJSON_IsString(sections) || !cJSON_IsString(location)) {
                throw std::runtime_error("NVS中的每日简报字段无效");
            }
            task.sections = sections->valuestring;
            task.location = location->valuestring;
        } else if (sections != nullptr || location != nullptr) {
            throw std::runtime_error("NVS中的普通定时任务包含简报字段");
        }
        task.trigger_at = static_cast<std::time_t>(trigger_at->valuedouble);
        if (weekdays != nullptr) {
            if (!cJSON_IsArray(weekdays)) {
                throw std::runtime_error("NVS中的weekdays无效");
            }
            cJSON* day = nullptr;
            cJSON_ArrayForEach(day, weekdays) {
                if (!cJSON_IsNumber(day) || day->valueint < 1 || day->valueint > 7) {
                    throw std::runtime_error("NVS中的weekday超出1到7");
                }
                task.weekdays.push_back(day->valueint);
            }
        }
        restored.push_back(std::move(task));
    }
    const uint32_t restored_next_id = static_cast<uint32_t>(next_id->valuedouble);
    schedule_manager_.Restore(std::move(restored), restored_next_id);
}

void Application::SaveSchedules() const {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "next_id", schedule_manager_.next_id());
    cJSON* tasks = cJSON_AddArrayToObject(root, "tasks");
    for (const auto& task : schedule_manager_.tasks()) {
        cJSON* item = ScheduleTaskJson(task);
        cJSON_ReplaceItemInObject(item, "trigger_at", cJSON_CreateNumber(task.trigger_at));
        cJSON_AddItemToArray(tasks, item);
    }
    const std::string saved = JsonString(root);
    static constexpr size_t kChunkSize = 1800;
    const int chunk_count = static_cast<int>((saved.size() + kChunkSize - 1) / kChunkSize);
    if (chunk_count > 8) throw std::runtime_error("定时任务JSON超过NVS容量限制");
    Settings settings("schedule", true);
    const int old_chunk_count = settings.GetInt("chunks", 0);
    settings.SetInt("chunks", chunk_count);
    for (int i = 0; i < chunk_count; ++i) {
        settings.SetString("data" + std::to_string(i),
                           saved.substr(i * kChunkSize, kChunkSize));
    }
    for (int i = chunk_count; i < old_chunk_count && i < 8; ++i) {
        settings.EraseKey("data" + std::to_string(i));
    }
    settings.EraseKey("tasks");
}

void Application::LoadProactive() {
    Settings settings("proactive");
    std::string saved;
    const int chunk_count = settings.GetInt("chunks", 0);
    if (chunk_count < 0 || chunk_count > 4) throw std::runtime_error("NVS中的主动状态分片数无效");
    for (int i = 0; i < chunk_count; ++i) {
        const auto chunk = settings.GetString("data" + std::to_string(i));
        if (chunk.empty()) throw std::runtime_error("NVS中的主动状态分片缺失");
        saved += chunk;
    }
    if (saved.empty()) {
        proactive_manager_.Restore({}, {});
        return;
    }
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(cJSON_Parse(saved.c_str()), cJSON_Delete);
    auto config_json = cJSON_GetObjectItem(root.get(), "config");
    auto follow_ups = cJSON_GetObjectItem(root.get(), "follow_ups");
    auto health = cJSON_GetObjectItem(root.get(), "health");
    auto queue = cJSON_GetObjectItem(root.get(), "queue");
    if (!cJSON_IsObject(root.get()) || !cJSON_IsObject(config_json) ||
        !cJSON_IsArray(follow_ups) || !cJSON_IsObject(health) || !cJSON_IsArray(queue)) {
        throw std::runtime_error("NVS中的主动状态JSON损坏");
    }
    auto mode = cJSON_GetObjectItem(config_json, "mode");
    auto previous_mode = cJSON_GetObjectItem(config_json, "mode_before_silent");
    auto silent_date = cJSON_GetObjectItem(config_json, "silent_date");
    auto daily_limit = cJSON_GetObjectItem(config_json, "daily_limit");
    auto allowed = cJSON_GetObjectItem(config_json, "allowed_topics");
    auto blocked = cJSON_GetObjectItem(config_json, "blocked_topics");
    auto budget_date = cJSON_GetObjectItem(config_json, "budget_date");
    auto delivered = cJSON_GetObjectItem(config_json, "delivered_today");
    auto cooldowns = cJSON_GetObjectItem(config_json, "last_delivered");
    if (!cJSON_IsString(mode) || !cJSON_IsString(previous_mode) ||
        !cJSON_IsNumber(silent_date) || !cJSON_IsNumber(daily_limit) ||
        !cJSON_IsArray(allowed) || !cJSON_IsArray(blocked) ||
        !cJSON_IsNumber(budget_date) || !cJSON_IsNumber(delivered) ||
        !cJSON_IsObject(cooldowns)) {
        throw std::runtime_error("NVS中的主动配置字段无效");
    }
    proactive::Config config;
    config.mode = proactive::Manager::ParseMode(mode->valuestring);
    config.mode_before_silent = proactive::Manager::ParseMode(previous_mode->valuestring);
    config.silent_date = silent_date->valueint;
    config.daily_limit = daily_limit->valueint;
    auto quiet_start = cJSON_GetObjectItem(config_json, "quiet_start");
    auto quiet_end = cJSON_GetObjectItem(config_json, "quiet_end");
    if ((quiet_start == nullptr) != (quiet_end == nullptr) ||
        (quiet_start && (!cJSON_IsNumber(quiet_start) || !cJSON_IsNumber(quiet_end)))) {
        throw std::runtime_error("NVS中的安静时段无效");
    }
    if (quiet_start) {
        config.quiet_start = quiet_start->valueint;
        config.quiet_end = quiet_end->valueint;
    }
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, allowed) {
        if (!cJSON_IsString(item)) throw std::runtime_error("NVS中的允许主题无效");
        config.allowed_topics.insert(item->valuestring);
    }
    cJSON_ArrayForEach(item, blocked) {
        if (!cJSON_IsString(item)) throw std::runtime_error("NVS中的屏蔽主题无效");
        config.blocked_topics.insert(item->valuestring);
    }
    proactive::RuntimeState runtime;
    runtime.budget_date = budget_date->valueint;
    runtime.delivered_today = delivered->valueint;
    cJSON_ArrayForEach(item, cooldowns) {
        if (!cJSON_IsNumber(item)) throw std::runtime_error("NVS中的主题冷却无效");
        runtime.last_delivered[item->string] = static_cast<std::time_t>(item->valuedouble);
    }
    proactive_manager_.Restore(std::move(config), std::move(runtime));

    std::vector<proactive::FollowUp> restored_follow_ups;
    cJSON_ArrayForEach(item, follow_ups) {
        auto source_id = cJSON_GetObjectItem(item, "source_id");
        auto label = cJSON_GetObjectItem(item, "label");
        auto triggered_at = cJSON_GetObjectItem(item, "source_triggered_at");
        auto due_at = cJSON_GetObjectItem(item, "due_at");
        auto expires_at = cJSON_GetObjectItem(item, "expires_at");
        auto asked = cJSON_GetObjectItem(item, "asked");
        if (!cJSON_IsNumber(source_id) || !cJSON_IsString(label) ||
            !cJSON_IsNumber(triggered_at) || !cJSON_IsNumber(due_at) ||
            !cJSON_IsNumber(expires_at) || !cJSON_IsBool(asked)) {
            throw std::runtime_error("NVS中的追问字段无效");
        }
        restored_follow_ups.push_back({static_cast<uint32_t>(source_id->valuedouble),
            label->valuestring, static_cast<std::time_t>(triggered_at->valuedouble),
            static_cast<std::time_t>(due_at->valuedouble),
            static_cast<std::time_t>(expires_at->valuedouble), cJSON_IsTrue(asked)});
    }
    schedule_follow_ups_.Restore(std::move(restored_follow_ups));

    std::map<std::string, proactive::HealthTracker::Record> health_records;
    cJSON_ArrayForEach(item, health) {
        auto active = cJSON_GetObjectItem(item, "active");
        auto changed = cJSON_GetObjectItem(item, "last_changed_at");
        auto dedupe = cJSON_GetObjectItem(item, "dedupe_key");
        if (!cJSON_IsObject(item) || !cJSON_IsBool(active) || !cJSON_IsNumber(changed) ||
            !cJSON_IsString(dedupe)) throw std::runtime_error("NVS中的健康去重字段无效");
        health_records[item->string] = {cJSON_IsTrue(active),
            static_cast<std::time_t>(changed->valuedouble), dedupe->valuestring};
    }
    health_tracker_.Restore(std::move(health_records));
    std::vector<proactive::Event> queued;
    cJSON_ArrayForEach(item, queue) queued.push_back(ParseProactiveEvent(item));
    for (const auto& event : queued) {
        auto source = event.metadata.find("source_id");
        if (source != event.metadata.end()) {
            FindFollowUpLabel(static_cast<uint32_t>(
                std::strtoul(source->second.c_str(), nullptr, 10)));
        }
    }
    proactive_queue_.Restore(std::move(queued));
}

void Application::SaveProactive() const {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "config",
        ProactiveConfigJson(proactive_manager_.config(), proactive_manager_.state()));
    cJSON* follow_ups = cJSON_AddArrayToObject(root, "follow_ups");
    for (const auto& follow_up : schedule_follow_ups_.items()) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "source_id", follow_up.source_id);
        cJSON_AddStringToObject(item, "label", follow_up.label.c_str());
        cJSON_AddNumberToObject(item, "source_triggered_at", follow_up.source_triggered_at);
        cJSON_AddNumberToObject(item, "due_at", follow_up.due_at);
        cJSON_AddNumberToObject(item, "expires_at", follow_up.expires_at);
        cJSON_AddBoolToObject(item, "asked", follow_up.asked);
        cJSON_AddItemToArray(follow_ups, item);
    }
    cJSON* health = cJSON_AddObjectToObject(root, "health");
    for (const auto& [kind, record] : health_tracker_.records()) {
        cJSON* item = cJSON_AddObjectToObject(health, kind.c_str());
        cJSON_AddBoolToObject(item, "active", record.active);
        cJSON_AddNumberToObject(item, "last_changed_at", record.last_changed_at);
        cJSON_AddStringToObject(item, "dedupe_key", record.dedupe_key.c_str());
    }
    cJSON* queue = cJSON_AddArrayToObject(root, "queue");
    for (const auto& event : proactive_queue_.items()) {
        cJSON_AddItemToArray(queue, ProactiveEventJson(event));
    }
    if (pending_proactive_event_) {
        cJSON_AddItemToArray(queue, ProactiveEventJson(*pending_proactive_event_));
    }
    const std::string saved = JsonString(root);
    static constexpr size_t kChunkSize = 1800;
    static constexpr size_t kMaxSerializedBytes = 7200;
    if (saved.size() > kMaxSerializedBytes) {
        throw std::runtime_error("主动状态超过7200字节持久化预算");
    }
    const int chunk_count = static_cast<int>((saved.size() + kChunkSize - 1) / kChunkSize);
    if (chunk_count > 4) throw std::runtime_error("主动状态JSON超过NVS容量限制");
    nvs_stats_t nvs_stats{};
    const esp_err_t stats_error = nvs_get_stats(nullptr, &nvs_stats);
    if (stats_error != ESP_OK) throw std::runtime_error("无法读取NVS剩余容量");
    size_t required_entries = 1;  // chunks i32
    for (int i = 0; i < chunk_count; ++i) {
        const size_t chunk_length = std::min(kChunkSize, saved.size() - i * kChunkSize);
        required_entries += 2 + (chunk_length + 32) / 32;
    }
    static constexpr size_t kNvsSafetyEntries = 16;
    if (nvs_stats.available_entries < required_entries + kNvsSafetyEntries) {
        throw std::runtime_error("NVS剩余空间不足，无法原子保存主动状态");
    }
    nvs_handle_t handle = 0;
    auto check_nvs = [&handle](esp_err_t error, const char* operation) {
        if (error == ESP_OK || (strcmp(operation, "erase") == 0 &&
                                error == ESP_ERR_NVS_NOT_FOUND)) return;
        if (handle != 0) nvs_close(handle);
        handle = 0;
        throw std::runtime_error(std::string("主动状态NVS") + operation + "失败: " +
                                 esp_err_to_name(error));
    };
    check_nvs(nvs_open("proactive", NVS_READWRITE, &handle), "open");
    int32_t old_count = 0;
    const esp_err_t get_count_error = nvs_get_i32(handle, "chunks", &old_count);
    if (get_count_error != ESP_OK && get_count_error != ESP_ERR_NVS_NOT_FOUND) {
        check_nvs(get_count_error, "read");
    }
    for (int i = 0; i < chunk_count; ++i) {
        const std::string key = "data" + std::to_string(i);
        const std::string chunk = saved.substr(i * kChunkSize, kChunkSize);
        check_nvs(nvs_set_str(handle, key.c_str(), chunk.c_str()), "write");
    }
    for (int i = chunk_count; i < old_count && i < 16; ++i) {
        const std::string key = "data" + std::to_string(i);
        check_nvs(nvs_erase_key(handle, key.c_str()), "erase");
    }
    check_nvs(nvs_set_i32(handle, "chunks", chunk_count), "write");
    check_nvs(nvs_commit(handle), "commit");
    nvs_close(handle);
}

bool Application::TrySaveProactive(const char* context, bool force) {
    const int64_t now_us = esp_timer_get_time();
    if (!force && !proactive_save_backoff_.Ready(now_us)) {
        proactive_save_pending_ = true;
        return false;
    }
    try {
        SaveProactive();
        proactive_save_pending_ = false;
        proactive_save_backoff_.OnSuccess();
        return true;
    } catch (const std::exception& error) {
        proactive_save_pending_ = true;
        proactive_save_backoff_.OnFailure(now_us);
        ESP_LOGE(TAG, "Cannot save proactive state (%s): %s", context, error.what());
        if (!schedule_alert_active_ && GetDeviceState() == kDeviceStateIdle) {
            Board::GetInstance().GetDisplay()->ShowNotification("主动状态保存失败，将自动重试", 5000);
        }
        return false;
    }
}

void Application::QueueHealthEvent(const proactive::HealthEvent& health) {
    if (!proactive::HealthTracker::IsSupportedKind(health.kind)) {
        ESP_LOGE(TAG, "Rejected unsupported health event kind=%s", health.kind.c_str());
        Board::GetInstance().GetDisplay()->ShowNotification("不支持的设备健康事件", 5000);
        return;
    }
    auto event = health.event;
    event.metadata["health_kind"] = health.kind;
    event.metadata["severity"] = proactive::HealthTracker::SeverityName(health.severity);
    event.metadata["recovered"] = health.recovered ? "true" : "false";
    for (const auto& [key, value] : health.details) {
        event.metadata["detail." + key] = value;
    }
    if (!has_server_time_.load()) event.expires_at = 0;
    try {
        auto evicted = proactive_queue_.Push(event);
        if (evicted) {
            ESP_LOGW(TAG, "Evicted lower priority proactive event id=%s for health event",
                     evicted->event_id.c_str());
        }
    } catch (const std::exception& error) {
        ESP_LOGE(TAG, "Cannot persist health event: %s", error.what());
        Board::GetInstance().GetDisplay()->ShowNotification("健康事件存储已满", 5000);
        return;
    }
    Board::GetInstance().GetDisplay()->ShowNotification(
        health.recovered ? "设备状态已恢复" : "检测到设备健康事件", 5000);
    if (!schedule_alert_active_ && GetDeviceState() == kDeviceStateIdle &&
        audio_service_.IsPlaybackIdle()) {
        audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
    }
    TrySaveProactive("health event", true);
}

bool Application::SendProactiveEvent(const proactive::Event& event) {
    if (IsProactiveConnectionBusy() || !network_connected_.load() ||
        protocol_ == nullptr) return false;
    if (!protocol_->IsAudioChannelOpened()) return false;
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "jsonrpc", "2.0");
    const bool health = event.metadata.count("health_kind") != 0;
    const bool follow_up = event.metadata.count("source_id") != 0;
    cJSON_AddStringToObject(root, "method", health ? "notifications/device/health" :
                            (follow_up ? "notifications/schedule/follow_up" :
                                         "notifications/assistant/proactive"));
    cJSON* params = cJSON_AddObjectToObject(root, "params");
    cJSON_AddNumberToObject(params, "version", 1);
    cJSON_AddStringToObject(params, "event_id", event.event_id.c_str());
    cJSON_AddStringToObject(params, "topic", event.topic.c_str());
    cJSON_AddStringToObject(params, "priority", PriorityName(event.priority));
    cJSON_AddStringToObject(params, "reason", event.reason.c_str());
    cJSON_AddNumberToObject(params, "created_at", event.created_at);
    cJSON_AddNumberToObject(params, "expires_at", event.expires_at);
    cJSON_AddStringToObject(params, "dedupe_key", event.dedupe_key.c_str());
    cJSON_AddBoolToObject(params, "requires_response", event.requires_response);
    if (follow_up) {
        const uint32_t source_id = static_cast<uint32_t>(
            std::strtoul(event.metadata.at("source_id").c_str(), nullptr, 10));
        const std::string label = FindFollowUpLabel(source_id);
        cJSON_AddBoolToObject(params, "follow_up", true);
        cJSON_AddNumberToObject(params, "source_id", source_id);
        cJSON_AddStringToObject(params, "label", label.c_str());
        cJSON_AddBoolToObject(params, "speak", true);
    }
    if (health) {
        cJSON_AddStringToObject(params, "kind", event.metadata.at("health_kind").c_str());
        cJSON_AddStringToObject(params, "severity", event.metadata.at("severity").c_str());
        cJSON_AddNumberToObject(params, "occurred_at", event.created_at);
        cJSON_AddBoolToObject(params, "recovered",
                              event.metadata.at("recovered") == "true");
        cJSON* details = cJSON_AddObjectToObject(params, "details");
        for (const auto& [key, value] : event.metadata) {
            if (key.rfind("detail.", 0) == 0) {
                cJSON_AddStringToObject(details, key.substr(7).c_str(), value.c_str());
            }
        }
    }
    return protocol_->SendMcpMessage(JsonString(root));
}

std::string Application::FindFollowUpLabel(uint32_t source_id) const {
    for (const auto& item : schedule_follow_ups_.items()) {
        if (item.source_id == source_id) return item.label;
    }
    throw std::runtime_error("追问事件缺少对应的原提醒");
}

void Application::RecordProactiveSendResult(bool success) {
    if (success) {
        proactive_retry_backoff_.OnSuccess();
        return;
    }
    proactive_retry_backoff_.OnFailure(esp_timer_get_time());
}

bool Application::StartProactiveConnectionWorker() {
    if (IsProactiveConnectionBusy() || protocol_ == nullptr ||
        !network_connected_.load() ||
        GetDeviceState() != kDeviceStateIdle || schedule_alert_active_ ||
        protocol_->IsAudioChannelOpened()) {
        return false;
    }
    bool expected = false;
    // StartProactiveConnectionWorker and destruction both run on the main task,
    // so destruction cannot enter between this busy publication and xTaskCreate.
    if (!proactive_connection_busy_.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        return false;
    }
    xSemaphoreTake(proactive_connection_done_, 0);
    BaseType_t result = xTaskCreate(
        [](void* arg) {
            auto* app = static_cast<Application*>(arg);
            app->ProactiveConnectionTask(app->proactive_protocol_generation_);
            vTaskDelete(nullptr);
        },
        "proactive_conn", 4096 * 2, this, 11, &proactive_connection_task_handle_);
    if (result != pdPASS) {
        proactive_connection_task_handle_ = nullptr;
        proactive_connection_busy_.store(false, std::memory_order_release);
        RecordProactiveSendResult(false);
        ESP_LOGE(TAG, "Failed to create proactive connection worker");
        return false;
    }
    return true;
}

void Application::ProactiveConnectionTask(uint32_t protocol_generation) {
    bool success = false;
    if (!proactive_shutdown_.load() && protocol_ != nullptr &&
        protocol_generation == proactive_protocol_generation_) {
        success = protocol_->OpenAudioChannel();
    }
    if (!proactive_shutdown_.load()) {
        auto shutdown_token = proactive_shutdown_token_;
        Schedule([this, shutdown_token, success, protocol_generation]() {
            if (!shutdown_token->load()) {
                FinishProactiveConnection(success, protocol_generation);
            }
        });
    }
    // busy remains true until the main-task Finish callback consumes this
    // signal. This Give is the worker's final Application access; the task
    // entry only calls vTaskDelete after this method returns.
    xSemaphoreGive(proactive_connection_done_);
}

void Application::FinishProactiveConnection(bool success, uint32_t protocol_generation) {
    if (xSemaphoreTake(proactive_connection_done_, 0) != pdTRUE) {
        proactive_finish_pending_ = true;
        proactive_finish_success_ = success;
        proactive_finish_generation_ = protocol_generation;
        xEventGroupSetBits(event_group_, MAIN_EVENT_CLOCK_TICK);
        return;
    }
    proactive_finish_pending_ = false;
    proactive_connection_task_handle_ = nullptr;
    proactive_connection_busy_.store(false, std::memory_order_release);
    if (protocol_generation == proactive_protocol_generation_) {
        RecordProactiveSendResult(success);
    }
    const bool reset_pending = proactive_reset_pending_;
    proactive_reset_pending_ = false;
    const bool close_pending = proactive_close_pending_ || !network_connected_.load();
    proactive_close_pending_ = false;
    const bool reboot_pending = proactive_reboot_pending_;
    proactive_reboot_pending_ = false;
    const auto deferred_open = proactive_deferred_open_mode_;
    proactive_deferred_open_mode_.reset();
    const auto deferred_wake_word = proactive_deferred_wake_word_;
    proactive_deferred_wake_word_.reset();
    const EventBits_t deferred_bits = proactive_deferred_event_bits_;
    proactive_deferred_event_bits_ = 0;
    if (reset_pending) {
        ResetProtocol();
        return;
    }
    if (reboot_pending) {
        Reboot();
        return;
    }
    if (close_pending && protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }
    while (!proactive_deferred_mcp_messages_.empty()) {
        auto payload = std::move(proactive_deferred_mcp_messages_.front());
        proactive_deferred_mcp_messages_.pop_front();
        if (protocol_) protocol_->SendMcpMessage(payload);
    }
    if (deferred_wake_word && GetDeviceState() == kDeviceStateIdle) {
        BeginWakeWordInvoke(*deferred_wake_word);
    } else if (deferred_wake_word && GetDeviceState() == kDeviceStateConnecting) {
        ContinueWakeWordInvoke(*deferred_wake_word);
    } else if (deferred_open && GetDeviceState() == kDeviceStateConnecting) {
        ContinueOpenAudioChannel(*deferred_open);
    }
    xEventGroupSetBits(event_group_, deferred_bits | MAIN_EVENT_CLOCK_TICK |
                                     MAIN_EVENT_PLAYBACK_DRAINED |
                                     MAIN_EVENT_SEND_AUDIO);
}

void Application::CheckProactiveEvents() {
    if (proactive_finish_pending_) {
        const bool success = proactive_finish_success_;
        const uint32_t generation = proactive_finish_generation_;
        proactive_finish_pending_ = false;
        FinishProactiveConnection(success, generation);
        if (IsProactiveConnectionBusy()) return;
    }
    const auto now = std::time(nullptr);
    bool changed = false;
    if (proactive_save_pending_ && proactive_save_backoff_.Ready(esp_timer_get_time())) {
        TrySaveProactive("scheduled retry");
    }
    if (has_server_time_.load()) {
        const auto mode_before_refresh = proactive_manager_.config().mode;
        proactive::Event date_refresh{"date-refresh", "maintenance",
            proactive::Priority::kLow, "refresh local date", now, now,
            "date-refresh", false, {}};
        proactive_manager_.ShouldDeliver(date_refresh, now, true);
        changed = mode_before_refresh != proactive_manager_.config().mode || changed;
        if (network_connected_.load()) {
            if (auto recovered = health_tracker_.Recover("network_flapping", now)) {
                QueueHealthEvent(*recovered);
            }
        }
    }
    if (!has_server_time_.load() && uptime_ticks_ >= 600 && !time_unsynced_health_reported_) {
        time_unsynced_health_reported_ = true;
        if (auto event = health_tracker_.Raise(
                "time_unsynchronized", proactive::Severity::kWarning, now,
                {{"uptime_seconds", std::to_string(uptime_ticks_)}})) {
            QueueHealthEvent(*event);
        }
    }
    changed = schedule_follow_ups_.DropExpired(now) > 0 || changed;
    if (has_server_time_.load() && follow_up_enqueue_backoff_.Ready(esp_timer_get_time())) {
        for (const auto& follow_up : schedule_follow_ups_.PendingDue(now)) {
            proactive::Event event{
                "follow-up-" + std::to_string(follow_up.source_id) + "-" +
                    std::to_string(follow_up.due_at),
                "follow_up", proactive::Priority::kHigh,
                "confirm reminder completion", now, follow_up.expires_at,
                "follow-up:" + std::to_string(follow_up.source_id), true, {}};
            event.metadata["source_id"] = std::to_string(follow_up.source_id);
            const auto old_follow_ups = schedule_follow_ups_.items();
            const auto old_queue = proactive_queue_.items();
            try {
                auto evicted = proactive_queue_.Push(event);
                if (evicted) {
                    ESP_LOGW(TAG, "Evicted proactive event id=%s for due follow-up",
                             evicted->event_id.c_str());
                }
                schedule_follow_ups_.MarkAsked(follow_up.source_id, follow_up.due_at);
                if (!TrySaveProactive("due follow-up transaction", true)) {
                    schedule_follow_ups_.Restore(old_follow_ups);
                    proactive_queue_.Restore(old_queue);
                    follow_up_enqueue_backoff_.OnFailure(esp_timer_get_time());
                    ESP_LOGE(TAG, "Rolled back due follow-up id=%lu after save failure",
                             static_cast<unsigned long>(follow_up.source_id));
                    break;
                }
                follow_up_enqueue_backoff_.OnSuccess();
            } catch (const std::exception& error) {
                schedule_follow_ups_.Restore(old_follow_ups);
                proactive_queue_.Restore(old_queue);
                ESP_LOGE(TAG, "Cannot enqueue due follow-up id=%lu: %s",
                         static_cast<unsigned long>(follow_up.source_id), error.what());
                follow_up_enqueue_backoff_.OnFailure(esp_timer_get_time());
                break;
            }
        }
    }
    if (proactive_queue_.DropExpired(now) > 0) changed = true;
    if (IsProactiveConnectionBusy()) {
        if (changed) TrySaveProactive("proactive tick while connecting");
        return;
    }
    if (!pending_proactive_event_ && !schedule_alert_active_ &&
        proactive_retry_backoff_.Ready(esp_timer_get_time()) &&
        GetDeviceState() != kDeviceStateSpeaking) {
        std::vector<proactive::Event> deferred;
        std::optional<proactive::Event> event;
        const size_t candidates = proactive_queue_.items().size();
        for (size_t i = 0; i < candidates; ++i) {
            auto candidate = proactive_queue_.PopNext(now);
            if (!candidate) break;
            const bool recovered_health =
                candidate->metadata.count("recovered") != 0 &&
                candidate->metadata.at("recovered") == "true";
            if (recovered_health ||
                proactive_manager_.ShouldDeliver(*candidate, now,
                                                  has_server_time_.load())) {
                event = std::move(candidate);
                break;
            }
            deferred.push_back(std::move(*candidate));
        }
        for (auto& deferred_event : deferred) {
            proactive_queue_.Push(std::move(deferred_event));
        }
        if (event) {
            if (protocol_ == nullptr || !protocol_->IsAudioChannelOpened()) {
                proactive_queue_.Push(*event);
                StartProactiveConnectionWorker();
                event.reset();
            }
            if (event && event->metadata.count("source_id") != 0 &&
                event->metadata.count("cue_played") == 0) {
                const uint32_t source_id = static_cast<uint32_t>(std::strtoul(
                    event->metadata.at("source_id").c_str(), nullptr, 10));
                Board::GetInstance().GetDisplay()->ShowNotification(
                    ("刚才提醒的" + FindFollowUpLabel(source_id) + "完成了吗？").c_str(),
                    10000);
                pending_proactive_event_ = std::move(*event);
                audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
                changed = true;
            } else if (event) {
                const bool sent = SendProactiveEvent(*event);
                RecordProactiveSendResult(sent);
                if (sent) {
                    const bool recovered_health =
                        event->metadata.count("recovered") != 0 &&
                        event->metadata.at("recovered") == "true";
                    if (!recovered_health) {
                        proactive_manager_.RecordDelivered(
                            *event, now, has_server_time_.load());
                    }
                    changed = true;
                } else {
                    proactive_queue_.Push(std::move(*event));
                }
            }
        }
    }
    if (changed) TrySaveProactive("proactive tick");
}

bool Application::NotifyReminderTriggered(const schedule::Task& task, std::time_t now) {
    if (IsProactiveConnectionBusy()) {
        proactive_deferred_event_bits_ |= MAIN_EVENT_PLAYBACK_DRAINED;
        return false;
    }
    if (protocol_ == nullptr) {
        ESP_LOGE(TAG, "Cannot deliver reminder notification: protocol is not initialized");
        return false;
    }
    if (!protocol_->IsAudioChannelOpened()) {
        Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        if (!protocol_->OpenAudioChannel()) {
            Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
            ESP_LOGE(TAG, "Cannot deliver reminder notification: failed to open audio channel");
            return false;
        }
    }

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "jsonrpc", "2.0");
    const bool briefing = task.kind == schedule::Kind::kBriefing;
    cJSON_AddStringToObject(root, "method", briefing ?
        "notifications/assistant/triggered" : "notifications/schedule/triggered");
    cJSON* params = cJSON_AddObjectToObject(root, "params");
    cJSON_AddNumberToObject(params, "version", 1);
    cJSON_AddNumberToObject(params, "id", task.id);
    if (briefing) {
        const std::string triggered_at = FormatLocalDateTime(task.trigger_at);
        std::string event_timestamp = triggered_at;
        event_timestamp.erase(std::remove(event_timestamp.begin(), event_timestamp.end(), '-'),
                              event_timestamp.end());
        event_timestamp.erase(std::remove(event_timestamp.begin(), event_timestamp.end(), ':'),
                              event_timestamp.end());
        const std::string event_id = std::to_string(task.id) + "-" + event_timestamp;
        cJSON_AddStringToObject(params, "event_id", event_id.c_str());
        cJSON_AddStringToObject(params, "workflow", "daily_briefing");
        cJSON* sections = cJSON_AddArrayToObject(params, "sections");
        if (task.sections.find("weather") != std::string::npos) {
            cJSON_AddItemToArray(sections, cJSON_CreateString("weather"));
        }
        if (task.sections.find("news") != std::string::npos) {
            cJSON_AddItemToArray(sections, cJSON_CreateString("news"));
        }
        cJSON_AddStringToObject(params, "location", task.location.c_str());
        cJSON_AddStringToObject(params, "triggered_at", triggered_at.c_str());
    } else {
        cJSON_AddStringToObject(params, "kind", schedule::Manager::KindName(task.kind));
        cJSON_AddStringToObject(params, "label", task.label.c_str());
        cJSON_AddStringToObject(params, "triggered_at", FormatLocalDateTime(now).c_str());
    }
    cJSON_AddBoolToObject(params, "speak", true);
    return protocol_->SendMcpMessage(JsonString(root));
}

void Application::CheckSchedules() {
    const std::time_t now = std::time(nullptr);
    auto result = schedule_manager_.Tick(now, has_server_time_.load());
    if (result.changed) SaveSchedules();
    if (!result.missed.empty()) {
        cJSON* missed = cJSON_CreateArray();
        for (const auto& task : result.missed) {
            ESP_LOGW(TAG, "Missed schedule id=%lu label=%s", static_cast<unsigned long>(task.id),
                     task.label.c_str());
            cJSON* record = cJSON_CreateObject();
            cJSON_AddNumberToObject(record, "id", task.id);
            cJSON_AddNumberToObject(record, "trigger_at", task.trigger_at);
            cJSON_AddItemToArray(missed, record);
        }
        Settings settings("schedule", true);
        settings.SetString("last_missed", JsonString(missed));
    }
    for (const auto& task : result.triggered) schedule_alert_queue_.Enqueue(task);
    const int64_t now_us = esp_timer_get_time();
    if (schedule_reminder_tts_deadline_us_ > 0 &&
        now_us >= schedule_reminder_tts_deadline_us_ &&
        reminder_delivery_.CancelWaitingForTts()) {
        schedule_reminder_tts_deadline_us_ = 0;
        AbortSpeaking(kAbortReasonNone);
        RestoreScheduleAlertVolumeAfterDelivery();
    }
    if (!schedule_alert_active_ &&
        reminder_delivery_.state() == schedule::ReminderDeliveryState::kInactive) {
        StartNextScheduleAlert();
    }
    if (!schedule_alert_active_) return;
    if (now_us >= schedule_alert_deadline_us_) {
        const bool reminder_delivery_pending =
            active_schedule_task_.kind != schedule::Kind::kAlarm &&
            reminder_delivery_.state() != schedule::ReminderDeliveryState::kInactive;
        FinishScheduleAlert(active_schedule_task_.kind == schedule::Kind::kAlarm,
                            !reminder_delivery_pending);
        if (!reminder_delivery_pending) {
            StartNextScheduleAlert();
        }
    } else if (active_schedule_task_.kind == schedule::Kind::kAlarm &&
               reminder_delivery_.state() != schedule::ReminderDeliveryState::kSpeaking &&
               GetDeviceState() != kDeviceStateSpeaking &&
               audio_service_.IsPlaybackIdle()) {
        audio_service_.PlaySound(Lang::Sounds::OGG_EXCLAMATION);
    }
}

void Application::StartNextScheduleAlert() {
    if (schedule_alert_active_) return;
    const auto* next = schedule_alert_queue_.StartNext();
    if (next == nullptr) return;
    active_schedule_task_ = *next;
    schedule_alert_active_ = true;
    schedule_alert_deadline_us_ = esp_timer_get_time() +
        (active_schedule_task_.kind == schedule::Kind::kAlarm ? 600LL : 60LL) * 1000000;
    reminder_delivery_.Begin();

    AbortSpeaking(kAbortReasonNone);
    SetDeviceState(kDeviceStateIdle);
    audio_service_.EnableVoiceProcessing(false);
    audio_service_.EnableWakeWordDetection(false);
    audio_service_.ResetDecoder();
    netease_lyrics_.Clear();
    Board::GetInstance().GetDisplay()->CloseNeteaseMusicLyrics();
    auto codec = Board::GetInstance().GetAudioCodec();
    schedule_saved_volume_ = codec ? codec->output_volume() : -1;
    schedule_volume_revision_ = codec ? codec->output_volume_revision() : 0;
    if (codec && schedule_saved_volume_ < kScheduleAlertMinimumVolume) {
        codec->SetOutputVolumeTransient(kScheduleAlertMinimumVolume);
    }

    ShowScheduleAlertPage();
    const bool is_reminder = active_schedule_task_.kind == schedule::Kind::kReminder;
    const int cue_repeats = is_reminder ? kReminderCueRepeats : 1;
    for (int i = 0; i < cue_repeats; ++i) {
        audio_service_.PlaySound(Lang::Sounds::OGG_EXCLAMATION);
    }
}

void Application::ShowScheduleAlertPage() {
    std::string page = active_schedule_task_.kind == schedule::Kind::kAlarm ? "闹铃" :
        (active_schedule_task_.kind == schedule::Kind::kReminder ? "提醒" : "每日简报");
    page += "  " + FormatLocalDateTime(active_schedule_task_.trigger_at).substr(11, 5) + "\n";
    page += active_schedule_task_.label;
    page += active_schedule_task_.kind == schedule::Kind::kBriefing ?
        "\n正在准备简报 / 开始收听键停止" :
        "\n开始收听键停止 / 可语音稍后提醒";
    Board::GetInstance().GetDisplay()->SetChatMessage("system", page.c_str());
}

void Application::RestoreScheduleAlertVolume() {
    if (schedule_saved_volume_ < 0) return;
    if (auto codec = Board::GetInstance().GetAudioCodec()) {
        if (schedule::ShouldRestoreTemporaryVolume(schedule_volume_revision_,
                                                   codec->output_volume_revision())) {
            codec->SetOutputVolumeTransient(schedule_saved_volume_);
        }
    }
    schedule_saved_volume_ = -1;
}

void Application::RestoreScheduleAlertVolumeAfterDelivery() {
    if (!schedule_alert_active_ ||
        active_schedule_task_.kind != schedule::Kind::kAlarm) {
        RestoreScheduleAlertVolume();
    }
}

void Application::FinishScheduleAlert(bool reset_decoder, bool reset_delivery) {
    if (!schedule_alert_active_) return;
    if (reset_decoder) {
        audio_service_.ResetDecoder();
    }
    if (reset_delivery) {
        reminder_delivery_.Reset();
        schedule_reminder_tts_deadline_us_ = 0;
    }
    RestoreScheduleAlertVolume();
    Board::GetInstance().GetDisplay()->ClearChatMessages();
    schedule_alert_queue_.Stop();
    schedule_alert_active_ = false;
}

std::string Application::CreateSchedule(const std::string& kind, const std::string& repeat,
                                        const std::string& label,
                                        const std::string& trigger_at, int delay_seconds,
                                        const std::string& weekdays,
                                        const std::string& sections,
                                        const std::string& location) {
    if (!has_server_time_.load()) throw std::runtime_error("设备时间尚未同步，请稍后重试");
    schedule::CreateRequest request;
    request.kind = schedule::Manager::ParseKind(kind);
    request.repeat = schedule::Manager::ParseRepeat(repeat);
    request.label = label;
    request.sections = sections == "news,weather" ? "weather,news" : sections;
    request.location = location;
    if (delay_seconds < 0) throw std::invalid_argument("delay_seconds必须大于0");
    request.trigger_at = trigger_at.empty() ? 0 : ParseLocalDateTime(trigger_at);
    request.delay_seconds = delay_seconds;
    request.weekdays = ParseWeekdays(weekdays);
    const auto now = std::time(nullptr);
    const auto task = schedule_manager_.Create(request, now);
    SaveSchedules();
    cJSON* data = cJSON_CreateObject();
    cJSON_AddItemToObject(data, "task", ScheduleTaskJson(task));
    std::string response = schedule::Manager::DescribeCreation(task, now);
    if (schedule::Manager::ShouldSuggestRepeating(schedule_manager_.tasks(), task)) {
        response += " 你已经多次设置相同时间和内容，要不要改成重复任务？";
        cJSON_AddStringToObject(
            data, "suggestion", "convert_to_repeating_schedule");
    }
    return ResponseEnvelope(response, data);
}

std::string Application::ListSchedules(const std::string& kind) const {
    const auto filter = schedule::Manager::ParseKindFilter(kind);
    const auto filtered = schedule_manager_.List(filter);
    cJSON* data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "kind", kind.c_str());
    cJSON* tasks = cJSON_AddArrayToObject(data, "tasks");
    std::string response;
    for (const auto& task : filtered) {
        cJSON_AddItemToArray(tasks, ScheduleTaskJson(task));
        if (!response.empty()) response += "；";
        response += schedule::Manager::DescribeTask(task);
    }
    const bool active_matches = schedule_alert_active_ &&
        (filter == schedule::KindFilter::kAll ||
         (filter == schedule::KindFilter::kAlarm &&
          active_schedule_task_.kind == schedule::Kind::kAlarm) ||
         (filter == schedule::KindFilter::kReminder &&
          active_schedule_task_.kind == schedule::Kind::kReminder) ||
         (filter == schedule::KindFilter::kBriefing &&
          active_schedule_task_.kind == schedule::Kind::kBriefing));
    cJSON_AddBoolToObject(data, "alert_active", active_matches);
    if (active_matches) {
        cJSON_AddItemToObject(data, "active", ScheduleTaskJson(active_schedule_task_));
        if (!response.empty()) response += "；";
        response += "当前正在响铃的" + schedule::Manager::DescribeTask(active_schedule_task_);
    }
    if (response.empty()) {
        response = std::string("当前没有未触发的") + KindFilterChinese(filter) + "。";
    } else {
        response = std::string("当前") + KindFilterChinese(filter) + "有：" + response + "。";
    }
    return ResponseEnvelope(response, data);
}

std::string Application::DeleteSchedule(uint32_t id) {
    const auto* found = schedule_manager_.Find(id);
    if (found == nullptr) throw std::invalid_argument("未找到指定id的定时任务");
    const auto deleted = *found;
    if (!schedule_manager_.Delete(id)) throw std::invalid_argument("未找到指定id的定时任务");
    SaveSchedules();
    cJSON* data = cJSON_CreateObject();
    cJSON_AddNumberToObject(data, "deleted_id", id);
    cJSON_AddStringToObject(data, "kind", schedule::Manager::KindName(deleted.kind));
    return ResponseEnvelope("已删除" + schedule::Manager::DescribeTask(deleted) + "。", data);
}

std::string Application::ClearSchedules(const std::string& kind) {
    const auto filter = schedule::Manager::ParseKindFilter(kind);
    const size_t count = schedule_manager_.Clear(filter);
    SaveSchedules();
    cJSON* data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "kind", kind.c_str());
    cJSON_AddNumberToObject(data, "cleared", count);
    return ResponseEnvelope("已清空" + std::to_string(count) + "个未触发的" +
                                KindFilterChinese(filter) + "。",
                            data);
}

std::string Application::StopScheduleAlert() {
    if (!schedule_alert_active_) throw std::runtime_error("当前没有正在触发的定时任务");
    const auto stopped = active_schedule_task_;
    if (!TryStopScheduleAlert()) throw std::runtime_error("当前没有正在触发的定时任务");
    cJSON* data = cJSON_CreateObject();
    cJSON_AddNumberToObject(data, "stopped_id", stopped.id);
    cJSON_AddStringToObject(data, "kind", schedule::Manager::KindName(stopped.kind));
    return ResponseEnvelope("已停止任务ID " + std::to_string(stopped.id) + "，类型" +
                                (stopped.kind == schedule::Kind::kAlarm ? "闹铃" :
                                 (stopped.kind == schedule::Kind::kReminder ? "提醒" :
                                  "每日简报")) +
                                "。",
                            data);
}

bool Application::TryStopScheduleAlert() {
    if (!schedule_alert_active_) return false;
    const bool aborting_delivery = reminder_delivery_.NeedsServerAbort();
    if (aborting_delivery) {
        AbortSpeaking(kAbortReasonNone);
    }
    FinishScheduleAlert();
    if (aborting_delivery) {
        SetDeviceState(kDeviceStateIdle);
    }
    StartNextScheduleAlert();
    return true;
}

std::string Application::SnoozeScheduleAlert(int minutes) {
    if (!schedule_alert_active_) throw std::runtime_error("当前没有可稍后提醒的闹铃或提醒");
    const auto previous = active_schedule_task_;
    const auto snoozed = schedule_manager_.Snooze(previous, minutes, std::time(nullptr));
    const bool aborting_delivery = reminder_delivery_.NeedsServerAbort();
    if (aborting_delivery) {
        AbortSpeaking(kAbortReasonNone);
    }
    FinishScheduleAlert();
    if (aborting_delivery) {
        SetDeviceState(kDeviceStateIdle);
    }
    SaveSchedules();
    StartNextScheduleAlert();
    cJSON* data = cJSON_CreateObject();
    cJSON_AddNumberToObject(data, "source_id", previous.id);
    cJSON_AddItemToObject(data, "task", ScheduleTaskJson(snoozed));
    return ResponseEnvelope("已延后" + std::to_string(minutes) + "分钟。", data);
}

std::string Application::ConfigureProactive(const std::string& mode, int daily_limit,
                                            const std::string& quiet_start,
                                            const std::string& quiet_end) {
    if ((quiet_start.empty()) != (quiet_end.empty())) {
        throw std::invalid_argument("quiet_start和quiet_end必须成对提供");
    }
    std::optional<int> start;
    std::optional<int> end;
    if (!quiet_start.empty()) {
        start = proactive::Manager::ParseClock(quiet_start);
        end = proactive::Manager::ParseClock(quiet_end);
    }
    const auto old_config = proactive_manager_.config();
    const auto old_state = proactive_manager_.state();
    proactive_manager_.Configure(proactive::Manager::ParseMode(mode),
                                 daily_limit < 0 ? std::nullopt : std::optional<int>(daily_limit),
                                 start, end);
    if (!TrySaveProactive("configure tool", true)) {
        proactive_manager_.Restore(old_config, old_state);
        proactive_save_pending_ = false;
        proactive_save_backoff_.OnSuccess();
        throw std::runtime_error("主动配置保存失败，未应用修改");
    }
    return ProactiveStatus();
}

std::string Application::ProactiveStatus() {
    const auto& config = proactive_manager_.config();
    cJSON* data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "mode", proactive::Manager::ModeName(config.mode));
    cJSON_AddNumberToObject(data, "daily_limit", config.daily_limit);
    cJSON_AddNumberToObject(data, "used_today", proactive_manager_.state().delivered_today);
    if (config.quiet_start) {
        cJSON_AddStringToObject(data, "quiet_start",
            proactive::Manager::FormatClock(*config.quiet_start).c_str());
        cJSON_AddStringToObject(data, "quiet_end",
            proactive::Manager::FormatClock(*config.quiet_end).c_str());
    } else {
        cJSON_AddNullToObject(data, "quiet_start");
        cJSON_AddNullToObject(data, "quiet_end");
    }
    cJSON* allowed = cJSON_AddArrayToObject(data, "allowed_topics");
    for (const auto& topic : config.allowed_topics) {
        cJSON_AddItemToArray(allowed, cJSON_CreateString(topic.c_str()));
    }
    cJSON* blocked = cJSON_AddArrayToObject(data, "blocked_topics");
    for (const auto& topic : config.blocked_topics) {
        cJSON_AddItemToArray(blocked, cJSON_CreateString(topic.c_str()));
    }
    return ResponseEnvelope("当前主动模式为" + std::string(proactive::Manager::ModeName(config.mode)) +
                                "，每天最多" + std::to_string(config.daily_limit) + "次。",
                            data);
}

std::string Application::MuteProactiveToday(const std::string& scope) {
    if (scope != "today") throw std::invalid_argument("scope目前只支持today");
    const auto old_config = proactive_manager_.config();
    const auto old_state = proactive_manager_.state();
    proactive_manager_.MuteToday(std::time(nullptr), has_server_time_.load());
    if (!TrySaveProactive("mute tool", true)) {
        proactive_manager_.Restore(old_config, old_state);
        proactive_save_pending_ = false;
        proactive_save_backoff_.OnSuccess();
        throw std::runtime_error("今日静默保存失败，未应用修改");
    }
    cJSON* data = ProactiveConfigJson(proactive_manager_.config(), proactive_manager_.state());
    return ResponseEnvelope("今天已安静，明天自动恢复。", data);
}

std::string Application::AllowProactiveTopic(const std::string& topic) {
    const auto old_config = proactive_manager_.config();
    const auto old_state = proactive_manager_.state();
    proactive_manager_.AllowTopic(topic);
    if (!TrySaveProactive("allow topic tool", true)) {
        proactive_manager_.Restore(old_config, old_state);
        proactive_save_pending_ = false;
        proactive_save_backoff_.OnSuccess();
        throw std::runtime_error("主题允许规则保存失败，未应用修改");
    }
    cJSON* data = ProactiveConfigJson(proactive_manager_.config(), proactive_manager_.state());
    return ResponseEnvelope("已允许该主题。", data);
}

std::string Application::BlockProactiveTopic(const std::string& topic) {
    const auto old_config = proactive_manager_.config();
    const auto old_state = proactive_manager_.state();
    proactive_manager_.BlockTopic(topic);
    if (!TrySaveProactive("block topic tool", true)) {
        proactive_manager_.Restore(old_config, old_state);
        proactive_save_pending_ = false;
        proactive_save_backoff_.OnSuccess();
        throw std::runtime_error("主题屏蔽规则保存失败，未应用修改");
    }
    cJSON* data = ProactiveConfigJson(proactive_manager_.config(), proactive_manager_.state());
    return ResponseEnvelope("已屏蔽该主题。", data);
}

std::string Application::CompleteRecentSchedule() {
    const auto old_follow_ups = schedule_follow_ups_.items();
    const auto old_queue = proactive_queue_.items();
    const auto old_pending = pending_proactive_event_;
    const auto completed = schedule_follow_ups_.CompleteRecent(std::time(nullptr));
    const std::string dedupe_key = "follow-up:" + std::to_string(completed.source_id);
    proactive_queue_.RemoveByDedupeKey(dedupe_key);
    if (pending_proactive_event_ && pending_proactive_event_->dedupe_key == dedupe_key) {
        pending_proactive_event_.reset();
    }
    if (!TrySaveProactive("complete recent tool", true)) {
        schedule_follow_ups_.Restore(old_follow_ups);
        proactive_queue_.Restore(old_queue);
        pending_proactive_event_ = old_pending;
        proactive_save_pending_ = false;
        proactive_save_backoff_.OnSuccess();
        throw std::runtime_error("完成状态保存失败，未应用修改");
    }
    cJSON* data = cJSON_CreateObject();
    cJSON_AddNumberToObject(data, "source_id", completed.source_id);
    return ResponseEnvelope("好的，已记为完成。", data);
}

std::string Application::FollowUpRecentSchedule(int minutes) {
    const auto old_follow_ups = schedule_follow_ups_.items();
    const auto old_queue = proactive_queue_.items();
    const auto old_pending = pending_proactive_event_;
    const auto delayed = schedule_follow_ups_.DelayRecent(minutes, std::time(nullptr));
    const std::string dedupe_key = "follow-up:" + std::to_string(delayed.source_id);
    proactive_queue_.RemoveByDedupeKey(dedupe_key);
    if (pending_proactive_event_ && pending_proactive_event_->dedupe_key == dedupe_key) {
        pending_proactive_event_.reset();
    }
    if (!TrySaveProactive("delay follow-up tool", true)) {
        schedule_follow_ups_.Restore(old_follow_ups);
        proactive_queue_.Restore(old_queue);
        pending_proactive_event_ = old_pending;
        proactive_save_pending_ = false;
        proactive_save_backoff_.OnSuccess();
        throw std::runtime_error("延后确认保存失败，未应用修改");
    }
    cJSON* data = cJSON_CreateObject();
    cJSON_AddNumberToObject(data, "source_id", delayed.source_id);
    cJSON_AddNumberToObject(data, "due_at", delayed.due_at);
    return ResponseEnvelope("好，" + std::to_string(minutes) + "分钟后再问。", data);
}

std::string Application::DismissScheduleFollowUp() {
    const auto old_follow_ups = schedule_follow_ups_.items();
    const auto old_queue = proactive_queue_.items();
    const auto old_pending = pending_proactive_event_;
    const auto dismissed = schedule_follow_ups_.DismissRecent(std::time(nullptr));
    const std::string dedupe_key = "follow-up:" + std::to_string(dismissed.source_id);
    proactive_queue_.RemoveByDedupeKey(dedupe_key);
    if (pending_proactive_event_ && pending_proactive_event_->dedupe_key == dedupe_key) {
        pending_proactive_event_.reset();
    }
    if (!TrySaveProactive("dismiss follow-up tool", true)) {
        schedule_follow_ups_.Restore(old_follow_ups);
        proactive_queue_.Restore(old_queue);
        pending_proactive_event_ = old_pending;
        proactive_save_pending_ = false;
        proactive_save_backoff_.OnSuccess();
        throw std::runtime_error("取消确认保存失败，未应用修改");
    }
    cJSON* data = cJSON_CreateObject();
    cJSON_AddNumberToObject(data, "source_id", dismissed.source_id);
    return ResponseEnvelope("好的，不再追问。", data);
}

void Application::ResetProtocol() {
    Schedule([this]() {
        if (IsProactiveConnectionBusy()) {
            proactive_reset_pending_ = true;
            ESP_LOGW(TAG, "Deferring protocol reset until proactive connection finishes");
            return;
        }
        ++proactive_protocol_generation_;
        // Close audio channel if opened
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
        // Reset protocol
        protocol_.reset();
    });
}
