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
#include <arpa/inet.h>
#include <cJSON.h>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>
#include <string_view>

#define TAG "Application"

namespace {

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
    cJSON_AddStringToObject(json, "trigger_at", FormatLocalDateTime(task.trigger_at).c_str());
    if (!task.weekdays.empty()) {
        cJSON* weekdays = cJSON_AddArrayToObject(json, "weekdays");
        for (int day : task.weekdays) cJSON_AddItemToArray(weekdays, cJSON_CreateNumber(day));
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
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    vEventGroupDelete(event_group_);
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
    LoadSchedules();
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddCommonTools();
    mcp_server.AddUserOnlyTools();

    // Set network event callback for UI updates and network state handling
    board.SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
        auto display = Board::GetInstance().GetDisplay();

        switch (event) {
            case NetworkEvent::Scanning:
                display->ShowNotification(Lang::Strings::SCANNING_WIFI, 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
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
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_CONNECTED);
                break;
            }
            case NetworkEvent::Disconnected:
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
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
            SetDeviceState(kDeviceStateIdle);
            Alert(Lang::Strings::ERROR, last_error_message_.c_str(), "cancel",
                  Lang::Sounds::OGG_EXCLAMATION);
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
            // Deferred listening start (auto mode): the playback queue has
            // drained, so it is now safe to enable voice processing.
            if (pending_listening_start_ && GetDeviceState() == kDeviceStateListening &&
                audio_service_.IsPlaybackIdle()) {
                pending_listening_start_ = false;
                StartListeningAudio();
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
            CheckSchedules();
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
    // Close current conversation when network disconnected
    auto state = GetDeviceState();
    if (state == kDeviceStateConnecting || state == kDeviceStateListening ||
        state == kDeviceStateSpeaking) {
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
            auto display = Board::GetInstance().GetDisplay();
            display->CloseNeteaseMusicLyrics();
            display->SetChatMessage("system", "");
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
                    aborted_ = false;
                    SetDeviceState(kDeviceStateSpeaking);
                });
            } else if (strcmp(state->valuestring, "stop") == 0) {
                Schedule([this]() {
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
            display->ClearChatMessages();    // Clear messages first
            display->SetEmotion("neutral");  // Then set emotion (wechat mode checks child count)
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(true);
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
    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();

    if (state == kDeviceStateIdle) {
        // May be called from outside the main task (e.g. board button
        // callbacks), so schedule the invocation instead of running it here
        Schedule([this, wake_word]() {
            if (GetDeviceState() == kDeviceStateIdle) {
                BeginWakeWordInvoke(wake_word);
            }
        });
    } else if (state == kDeviceStateSpeaking) {
        Schedule([this]() { AbortSpeaking(kAbortReasonNone); });
    } else if (state == kDeviceStateListening) {
        Schedule([this]() {
            if (protocol_) {
                protocol_->CloseAudioChannel();
            }
        });
    }
}

bool Application::CanEnterSleepMode() {
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
        if (protocol_) {
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
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
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
    cJSON* root = cJSON_Parse(saved.c_str());
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        throw std::runtime_error("NVS中的定时任务JSON损坏");
    }
    cJSON* next_id = cJSON_GetObjectItem(root, "next_id");
    cJSON* tasks = cJSON_GetObjectItem(root, "tasks");
    if (!cJSON_IsNumber(next_id) || next_id->valuedouble < 1 || !cJSON_IsArray(tasks)) {
        cJSON_Delete(root);
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
        if (!cJSON_IsNumber(id) || id->valuedouble < 1 || !cJSON_IsString(kind) ||
            !cJSON_IsString(repeat) || !cJSON_IsString(label) || !cJSON_IsNumber(trigger_at)) {
            cJSON_Delete(root);
            throw std::runtime_error("NVS中的定时任务条目无效");
        }
        schedule::Task task;
        task.id = static_cast<uint32_t>(id->valuedouble);
        task.kind = schedule::Manager::ParseKind(kind->valuestring);
        task.repeat = schedule::Manager::ParseRepeat(repeat->valuestring);
        task.label = label->valuestring;
        task.trigger_at = static_cast<std::time_t>(trigger_at->valuedouble);
        if (weekdays != nullptr) {
            if (!cJSON_IsArray(weekdays)) {
                cJSON_Delete(root);
                throw std::runtime_error("NVS中的weekdays无效");
            }
            cJSON* day = nullptr;
            cJSON_ArrayForEach(day, weekdays) {
                if (!cJSON_IsNumber(day) || day->valueint < 1 || day->valueint > 7) {
                    cJSON_Delete(root);
                    throw std::runtime_error("NVS中的weekday超出1到7");
                }
                task.weekdays.push_back(day->valueint);
            }
        }
        restored.push_back(std::move(task));
    }
    const uint32_t restored_next_id = static_cast<uint32_t>(next_id->valuedouble);
    cJSON_Delete(root);
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

void Application::NotifyReminderTriggered(const schedule::Task& task, std::time_t now) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "jsonrpc", "2.0");
    cJSON_AddStringToObject(root, "method", "notifications/schedule/triggered");
    cJSON* params = cJSON_AddObjectToObject(root, "params");
    cJSON_AddNumberToObject(params, "version", 1);
    cJSON_AddNumberToObject(params, "id", task.id);
    cJSON_AddStringToObject(params, "kind", schedule::Manager::KindName(task.kind));
    cJSON_AddStringToObject(params, "label", task.label.c_str());
    cJSON_AddStringToObject(params, "triggered_at", FormatLocalDateTime(now).c_str());
    cJSON_AddBoolToObject(params, "speak", true);
    SendMcpMessage(JsonString(root));
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
    if (!schedule_alert_active_) StartNextScheduleAlert();
    if (!schedule_alert_active_) return;
    if (now >= schedule_alert_deadline_) {
        FinishScheduleAlert();
        StartNextScheduleAlert();
    } else if (active_schedule_task_.kind == schedule::Kind::kAlarm &&
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
    const std::time_t now = std::time(nullptr);
    schedule_alert_deadline_ = now +
        (active_schedule_task_.kind == schedule::Kind::kAlarm ? 600 : 60);

    AbortSpeaking(kAbortReasonNone);
    audio_service_.ResetDecoder();
    auto codec = Board::GetInstance().GetAudioCodec();
    schedule_saved_volume_ = codec ? codec->output_volume() : -1;
    if (codec && schedule_saved_volume_ < 60) codec->SetOutputVolumeTransient(60);

    std::string page = active_schedule_task_.kind == schedule::Kind::kAlarm ? "闹铃" : "提醒";
    page += "  " + FormatLocalDateTime(active_schedule_task_.trigger_at).substr(11, 5) + "\n";
    page += active_schedule_task_.label;
    page += "\n按键停止 / 可语音稍后提醒";
    Board::GetInstance().GetDisplay()->SetChatMessage("system", page.c_str());
    audio_service_.PlaySound(Lang::Sounds::OGG_EXCLAMATION);
    if (active_schedule_task_.kind == schedule::Kind::kReminder) {
        NotifyReminderTriggered(active_schedule_task_, now);
    }
}

void Application::FinishScheduleAlert() {
    if (!schedule_alert_active_) return;
    audio_service_.ResetDecoder();
    if (schedule_saved_volume_ >= 0) {
        if (auto codec = Board::GetInstance().GetAudioCodec()) {
            codec->SetOutputVolumeTransient(schedule_saved_volume_);
        }
    }
    Board::GetInstance().GetDisplay()->ClearChatMessages();
    schedule_saved_volume_ = -1;
    schedule_alert_queue_.Stop();
    schedule_alert_active_ = false;
}

std::string Application::CreateSchedule(const std::string& kind, const std::string& repeat,
                                        const std::string& label,
                                        const std::string& trigger_at, int delay_seconds,
                                        const std::string& weekdays) {
    if (!has_server_time_.load()) throw std::runtime_error("设备时间尚未同步，请稍后重试");
    schedule::CreateRequest request;
    request.kind = schedule::Manager::ParseKind(kind);
    request.repeat = schedule::Manager::ParseRepeat(repeat);
    request.label = label;
    if (delay_seconds < 0) throw std::invalid_argument("delay_seconds必须大于0");
    request.trigger_at = trigger_at.empty() ? 0 : ParseLocalDateTime(trigger_at);
    request.delay_seconds = delay_seconds;
    request.weekdays = ParseWeekdays(weekdays);
    const auto task = schedule_manager_.Create(request, std::time(nullptr));
    SaveSchedules();
    cJSON* data = cJSON_CreateObject();
    cJSON_AddItemToObject(data, "task", ScheduleTaskJson(task));
    return ResponseEnvelope("已创建定时任务，以data.task为准。", data);
}

std::string Application::ListSchedules() const {
    cJSON* data = cJSON_CreateObject();
    cJSON* tasks = cJSON_AddArrayToObject(data, "tasks");
    for (const auto& task : schedule_manager_.tasks()) {
        cJSON_AddItemToArray(tasks, ScheduleTaskJson(task));
    }
    cJSON_AddBoolToObject(data, "alert_active", schedule_alert_active_);
    if (schedule_alert_active_) {
        cJSON_AddItemToObject(data, "active", ScheduleTaskJson(active_schedule_task_));
    }
    return ResponseEnvelope("已列出全部定时任务，以data为准。", data);
}

std::string Application::DeleteSchedule(uint32_t id) {
    if (!schedule_manager_.Delete(id)) throw std::invalid_argument("未找到指定id的定时任务");
    SaveSchedules();
    cJSON* data = cJSON_CreateObject();
    cJSON_AddNumberToObject(data, "deleted_id", id);
    return ResponseEnvelope("已删除定时任务。", data);
}

std::string Application::ClearSchedules() {
    const size_t count = schedule_manager_.Clear();
    SaveSchedules();
    cJSON* data = cJSON_CreateObject();
    cJSON_AddNumberToObject(data, "cleared", count);
    return ResponseEnvelope("已清空全部未触发的定时任务。", data);
}

std::string Application::StopScheduleAlert() {
    if (!schedule_alert_active_) throw std::runtime_error("当前没有正在响铃的闹铃或提醒");
    const uint32_t stopped_id = active_schedule_task_.id;
    FinishScheduleAlert();
    StartNextScheduleAlert();
    cJSON* data = cJSON_CreateObject();
    cJSON_AddNumberToObject(data, "stopped_id", stopped_id);
    return ResponseEnvelope("已停止当前闹铃或提醒。", data);
}

std::string Application::SnoozeScheduleAlert(int minutes) {
    if (!schedule_alert_active_) throw std::runtime_error("当前没有可稍后提醒的闹铃或提醒");
    const auto previous = active_schedule_task_;
    const auto snoozed = schedule_manager_.Snooze(previous, minutes, std::time(nullptr));
    FinishScheduleAlert();
    SaveSchedules();
    StartNextScheduleAlert();
    cJSON* data = cJSON_CreateObject();
    cJSON_AddNumberToObject(data, "source_id", previous.id);
    cJSON_AddItemToObject(data, "task", ScheduleTaskJson(snoozed));
    return ResponseEnvelope("已稍后提醒，以data.task为准。", data);
}

void Application::ResetProtocol() {
    Schedule([this]() {
        // Close audio channel if opened
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
        // Reset protocol
        protocol_.reset();
    });
}
