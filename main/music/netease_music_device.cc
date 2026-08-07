#include "netease_music_device.h"

#include "application.h"
#include "board.h"
#include "display.h"
#include "lvgl_display.h"
#include "lvgl_image.h"
#include "settings.h"
#include "system_info.h"

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>

#include <cstring>
#include <cerrno>
#include <cstdlib>
#include <limits>
#include <string_view>
#include <utility>

#define TAG "NeteaseMusic"

namespace netease_music {
namespace {

constexpr size_t kMaximumQrImageBytes = 256 * 1024;

std::string GetApiBaseUrl() {
#ifdef CONFIG_NETEASE_MUSIC_API_BASE_URL
    std::string base_url = CONFIG_NETEASE_MUSIC_API_BASE_URL;
#else
    std::string base_url;
#endif
    if (base_url.empty()) {
        Settings settings("wifi", false);
        base_url = settings.GetString("ota_url");
        if (base_url.empty()) {
            base_url = CONFIG_OTA_URL;
        }

        while (!base_url.empty() && base_url.back() == '/') {
            base_url.pop_back();
        }
        constexpr std::string_view kOtaSuffix = "/ota";
        if (base_url.size() >= kOtaSuffix.size() &&
            base_url.compare(base_url.size() - kOtaSuffix.size(), kOtaSuffix.size(),
                             kOtaSuffix) == 0) {
            base_url.resize(base_url.size() - kOtaSuffix.size());
        } else {
            base_url.clear();
        }
    }
    while (!base_url.empty() && base_url.back() == '/') {
        base_url.pop_back();
    }
    return base_url;
}

void SetDeviceApiHeaders(Http& http) {
    http.SetHeader("Content-Type", "application/json");
    http.SetHeader("Device-Id", SystemInfo::GetMacAddress());
    http.SetHeader("Client-Id", Board::GetInstance().GetUuid());

    Settings settings("websocket", false);
    std::string token = settings.GetString("token");
    if (!token.empty()) {
        if (token.find(' ') == std::string::npos) {
            token = "Bearer " + token;
        }
        http.SetHeader("Authorization", token);
    }
}

std::string EncodePathSegment(const std::string& value) {
    constexpr char kHex[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(value.size());
    for (const unsigned char ch : value) {
        const bool unreserved = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                                (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' ||
                                ch == '.' || ch == '~';
        if (unreserved) {
            encoded.push_back(static_cast<char>(ch));
        } else {
            encoded.push_back('%');
            encoded.push_back(kHex[ch >> 4]);
            encoded.push_back(kHex[ch & 0x0f]);
        }
    }
    return encoded;
}

bool ParseUint32(const cJSON* item, uint32_t& value) {
    if (cJSON_IsNumber(item)) {
        if (item->valuedouble < 0 || item->valuedouble > std::numeric_limits<uint32_t>::max()) {
            return false;
        }
        value = static_cast<uint32_t>(item->valuedouble);
        return true;
    }
    if (!cJSON_IsString(item) || item->valuestring == nullptr || item->valuestring[0] == '\0') {
        return false;
    }

    errno = 0;
    char* end = nullptr;
    const unsigned long long parsed = strtoull(item->valuestring, &end, 10);
    if (errno != 0 || end == item->valuestring || *end != '\0' ||
        parsed > std::numeric_limits<uint32_t>::max()) {
        return false;
    }
    value = static_cast<uint32_t>(parsed);
    return true;
}

class ManagerApiClient final : public Client {
public:
    Result<LoginStatus> GetLoginStatus() override {
        cJSON* root = Request("GET", "/device/netease/status");
        if (root == nullptr) {
            return Result<LoginStatus>::Failure(last_error_);
        }
        cJSON* data = GetData(root);
        cJSON* status = data == nullptr ? nullptr : cJSON_GetObjectItem(data, "status");
        Result<LoginStatus> result = Result<LoginStatus>::Failure("invalid login status response");
        if (cJSON_IsString(status) && strcmp(status->valuestring, "logged_in") == 0) {
            result = Result<LoginStatus>::Success(LoginStatus::kLoggedIn);
        } else if (cJSON_IsString(status) && strcmp(status->valuestring, "logged_out") == 0) {
            result = Result<LoginStatus>::Success(LoginStatus::kLoggedOut);
        }
        cJSON_Delete(root);
        return result;
    }

    Result<LoginSession> CreateLoginSession() override {
        cJSON* root = Request("POST", "/device/netease/sessions");
        if (root == nullptr) {
            return Result<LoginSession>::Failure(last_error_);
        }
        cJSON* data = GetData(root);
        cJSON* session_id = data == nullptr ? nullptr : cJSON_GetObjectItem(data, "session_id");
        cJSON* qr_image_url =
            data == nullptr ? nullptr : cJSON_GetObjectItem(data, "qr_image_url");
        cJSON* expires_in_ms =
            data == nullptr ? nullptr : cJSON_GetObjectItem(data, "expires_in_ms");
        cJSON* poll_interval_ms =
            data == nullptr ? nullptr : cJSON_GetObjectItem(data, "poll_interval_ms");

        uint32_t expires = 0;
        uint32_t poll_interval = 0;
        Result<LoginSession> result =
            Result<LoginSession>::Failure("invalid login session response");
        if (cJSON_IsString(session_id) && cJSON_IsString(qr_image_url) &&
            ParseUint32(expires_in_ms, expires) && expires > 0 &&
            ParseUint32(poll_interval_ms, poll_interval)) {
            result = Result<LoginSession>::Success(
                {.session_id = session_id->valuestring,
                 .qr_image_url = qr_image_url->valuestring,
                 .expires_in_ms = expires,
                 .poll_interval_ms = poll_interval});
        }
        cJSON_Delete(root);
        return result;
    }

    Result<AuthorizationStatus> GetAuthorizationStatus(
        const std::string& session_id) override {
        cJSON* root = Request("GET", "/device/netease/sessions/" + EncodePathSegment(session_id));
        if (root == nullptr) {
            return Result<AuthorizationStatus>::Failure(last_error_);
        }
        cJSON* data = GetData(root);
        cJSON* status = data == nullptr ? nullptr : cJSON_GetObjectItem(data, "status");
        Result<AuthorizationStatus> result =
            Result<AuthorizationStatus>::Failure("invalid authorization status response");
        if (cJSON_IsString(status)) {
            const std::string value = status->valuestring;
            if (value == "pending") {
                result = Result<AuthorizationStatus>::Success(AuthorizationStatus::kPending);
            } else if (value == "authorized") {
                result = Result<AuthorizationStatus>::Success(AuthorizationStatus::kAuthorized);
            } else if (value == "expired") {
                result = Result<AuthorizationStatus>::Success(AuthorizationStatus::kExpired);
            } else if (value == "cancelled") {
                result = Result<AuthorizationStatus>::Success(AuthorizationStatus::kCancelled);
            } else if (value == "failed") {
                result = Result<AuthorizationStatus>::Success(AuthorizationStatus::kFailed);
            }
        }
        cJSON_Delete(root);
        return result;
    }

    Result<LogoutStatus> Logout() override {
        cJSON* root = Request("POST", "/device/netease/logout");
        if (root == nullptr) {
            return Result<LogoutStatus>::Failure(last_error_);
        }
        cJSON* data = GetData(root);
        cJSON* status = data == nullptr ? nullptr : cJSON_GetObjectItem(data, "status");
        Result<LogoutStatus> result = Result<LogoutStatus>::Failure("invalid logout response");
        if (cJSON_IsString(status) && strcmp(status->valuestring, "logged_out") == 0) {
            result = Result<LogoutStatus>::Success(LogoutStatus::kLoggedOut);
        } else if (cJSON_IsString(status) &&
                   strcmp(status->valuestring, "already_logged_out") == 0) {
            result = Result<LogoutStatus>::Success(LogoutStatus::kAlreadyLoggedOut);
        }
        cJSON_Delete(root);
        return result;
    }

private:
    cJSON* Request(const char* method, const std::string& path) {
        const std::string base_url = GetApiBaseUrl();
        if (base_url.empty()) {
            last_error_ = "NetEase Music API base URL is not configured";
            ESP_LOGE(TAG, "%s", last_error_.c_str());
            return nullptr;
        }

        auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
        http->SetTimeout(10000);
        SetDeviceApiHeaders(*http);
        if (strcmp(method, "POST") == 0) {
            http->SetContent("{}");
        }
        if (!http->Open(method, base_url + path)) {
            last_error_ = "failed to connect to NetEase Music API";
            ESP_LOGE(TAG, "%s: transport error=0x%x", path.c_str(), http->GetLastError());
            return nullptr;
        }
        const int status_code = http->GetStatusCode();
        std::string body = http->ReadAll();
        http->Close();
        if (status_code != 200) {
            last_error_ = "NetEase Music API HTTP " + std::to_string(status_code);
            ESP_LOGE(TAG, "%s: HTTP %d", path.c_str(), status_code);
            return nullptr;
        }

        cJSON* root = cJSON_Parse(body.c_str());
        if (root == nullptr) {
            last_error_ = "invalid NetEase Music API JSON";
            ESP_LOGE(TAG, "%s: invalid JSON", path.c_str());
            return nullptr;
        }
        cJSON* code = cJSON_GetObjectItem(root, "code");
        if (!cJSON_IsNumber(code) || code->valueint != 0) {
            cJSON* message = cJSON_GetObjectItem(root, "msg");
            last_error_ = cJSON_IsString(message) ? message->valuestring
                                                  : "NetEase Music API business error";
            ESP_LOGW(TAG, "%s: business code=%d, message=%s", path.c_str(),
                     cJSON_IsNumber(code) ? code->valueint : -1, last_error_.c_str());
            cJSON_Delete(root);
            return nullptr;
        }
        ESP_LOGI(TAG, "%s %s: success", method, path.c_str());
        return root;
    }

    cJSON* GetData(cJSON* root) {
        cJSON* data = cJSON_GetObjectItem(root, "data");
        return cJSON_IsObject(data) ? data : nullptr;
    }

    std::string last_error_;
};

class DeviceScheduler final : public Scheduler {
public:
    DeviceScheduler() {
        esp_timer_create_args_t args = {
            .callback = [](void* context) {
                auto* self = static_cast<DeviceScheduler*>(context);
                Application::GetInstance().Schedule([self]() {
                    if (self->callback_) {
                        auto callback = std::move(self->callback_);
                        callback();
                    }
                });
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "netease_poll",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&args, &timer_));
    }

    ~DeviceScheduler() override {
        Cancel();
        if (timer_ != nullptr) {
            esp_timer_delete(timer_);
        }
    }

    void Schedule(uint32_t delay_ms, std::function<void()> callback) override {
        Cancel();
        callback_ = std::move(callback);
        esp_timer_start_once(timer_, static_cast<uint64_t>(delay_ms) * 1000);
    }

    void Cancel() override {
        callback_ = {};
        if (timer_ != nullptr && esp_timer_is_active(timer_)) {
            esp_timer_stop(timer_);
        }
    }

    uint64_t NowMs() const override { return esp_timer_get_time() / 1000; }

private:
    esp_timer_handle_t timer_ = nullptr;
    std::function<void()> callback_;
};

class DeviceObserver final : public Observer {
public:
    bool ShowQrImage(const std::string& image_url) override {
#ifdef HAVE_LVGL
        auto* display = dynamic_cast<LvglDisplay*>(Board::GetInstance().GetDisplay());
        if (display == nullptr) {
            return false;
        }

        auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
        http->SetTimeout(10000);
        SetDeviceApiHeaders(*http);
        if (!http->Open("GET", image_url)) {
            return false;
        }
        const int status_code = http->GetStatusCode();
        const size_t content_length = http->GetBodyLength();
        if (status_code != 200 || content_length == 0 || content_length > kMaximumQrImageBytes) {
            http->Close();
            ESP_LOGW(TAG, "Invalid QR image response: status=%d, length=%u", status_code,
                     static_cast<unsigned>(content_length));
            return false;
        }

        void* data = heap_caps_malloc(content_length, MALLOC_CAP_8BIT);
        if (data == nullptr) {
            http->Close();
            return false;
        }
        size_t total_read = 0;
        while (total_read < content_length) {
            const int read = http->Read(static_cast<char*>(data) + total_read,
                                        content_length - total_read);
            if (read <= 0) {
                break;
            }
            total_read += static_cast<size_t>(read);
        }
        http->Close();
        if (total_read != content_length) {
            heap_caps_free(data);
            return false;
        }

        try {
            if (!display->ShowNeteaseMusicQr(
                    std::make_unique<LvglAllocatedImage>(data, content_length),
                    "请使用网易云音乐扫码并确认")) {
                return false;
            }
        } catch (...) {
            heap_caps_free(data);
            return false;
        }
        qr_visible_ = true;
        return true;
#else
        (void)image_url;
        return false;
#endif
    }

    void UpdateQrStatus(const std::string& message) override {
#ifdef HAVE_LVGL
        auto* display = dynamic_cast<LvglDisplay*>(Board::GetInstance().GetDisplay());
        if (display != nullptr && qr_visible_) {
            display->UpdateNeteaseMusicQrStatus(message);
        }
#else
        (void)message;
#endif
    }

    void ClearQrImage() override {
        if (!qr_visible_) {
            return;
        }
#ifdef HAVE_LVGL
        auto* display = dynamic_cast<LvglDisplay*>(Board::GetInstance().GetDisplay());
        if (display != nullptr) {
            display->CloseNeteaseMusicQr();
        }
#endif
        qr_visible_ = false;
    }

    void ShowMessage(const std::string& message) override {
        Board::GetInstance().GetDisplay()->ShowNotification(message, 8000);
    }

    void RequestVoicePrompt(const std::string& message) override {
        cJSON* root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "jsonrpc", "2.0");
        cJSON_AddStringToObject(root, "method", "notifications/netease_music/status");
        cJSON* params = cJSON_CreateObject();
        cJSON_AddStringToObject(params, "message", message.c_str());
        cJSON_AddBoolToObject(params, "speak", true);
        cJSON_AddItemToObject(root, "params", params);
        char* json = cJSON_PrintUnformatted(root);
        if (json != nullptr) {
            Application::GetInstance().SendMcpMessage(json);
            cJSON_free(json);
        }
        cJSON_Delete(root);
    }

private:
    bool qr_visible_ = false;
};

}  // namespace

std::unique_ptr<Client> CreateNeteaseMusicClient() {
    return std::make_unique<ManagerApiClient>();
}

Service& GetDeviceService() {
    static auto client = CreateNeteaseMusicClient();
    static DeviceScheduler scheduler;
    static DeviceObserver observer;
    static Service service(*client, scheduler, observer);
    return service;
}

}  // namespace netease_music
