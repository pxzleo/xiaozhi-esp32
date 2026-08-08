#include "external_monitor_device.h"

#include "board.h"
#include "settings.h"
#include "system_info.h"

#include <cJSON.h>
#include <esp_log.h>

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <initializer_list>
#include <limits>
#include <memory>
#include <string_view>
#include <utility>

#define TAG "ExternalMonitor"

namespace external_monitor {
namespace {

constexpr size_t kMaximumPendingResponseBytes = 2048;

std::string GetManagerApiBaseUrl() {
    Settings settings("wifi", false);
    std::string base_url = settings.GetString("ota_url");
    if (base_url.empty()) base_url = CONFIG_OTA_URL;
    while (!base_url.empty() && base_url.back() == '/') base_url.pop_back();
    constexpr std::string_view kOtaSuffix = "/ota";
    if (base_url.size() < kOtaSuffix.size() ||
        base_url.compare(base_url.size() - kOtaSuffix.size(), kOtaSuffix.size(),
                         kOtaSuffix) != 0) {
        return {};
    }
    base_url.resize(base_url.size() - kOtaSuffix.size());
    return base_url;
}

void SetDeviceApiHeaders(Http& http) {
    http.SetHeader("Accept", "application/json");
    http.SetHeader("Device-Id", SystemInfo::GetMacAddress());
    http.SetHeader("Client-Id", Board::GetInstance().GetUuid());
    Settings settings("websocket", false);
    std::string token = settings.GetString("token");
    if (!token.empty()) {
        if (token.find(' ') == std::string::npos) token = "Bearer " + token;
        http.SetHeader("Authorization", token);
    }
}

bool HasExactFields(const cJSON* object, std::initializer_list<const char*> fields) {
    if (!cJSON_IsObject(object) || cJSON_GetArraySize(object) != static_cast<int>(fields.size())) {
        return false;
    }
    for (const char* field : fields) {
        if (cJSON_GetObjectItemCaseSensitive(object, field) == nullptr) return false;
    }
    return true;
}

bool ParseDateTime(const cJSON* item, std::time_t& timestamp) {
    if (!cJSON_IsString(item) || item->valuestring == nullptr ||
        std::strlen(item->valuestring) != 19) {
        return false;
    }
    return ParseManagerDateTime(item->valuestring, timestamp);
}

bool ParseInteger(const cJSON* item, int minimum, int maximum, int& value) {
    if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble) ||
        std::floor(item->valuedouble) != item->valuedouble ||
        item->valuedouble < minimum || item->valuedouble > maximum) {
        return false;
    }
    value = static_cast<int>(item->valuedouble);
    return true;
}

ProbeResult ParseResponse(const std::string& body) {
    const char* parse_end = nullptr;
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(
        cJSON_ParseWithOpts(body.c_str(), &parse_end, true), cJSON_Delete);
    if (!cJSON_IsObject(root.get())) return {.error = "pending响应不是JSON对象"};
    const cJSON* code = cJSON_GetObjectItemCaseSensitive(root.get(), "code");
    int result_code = -1;
    if (!ParseInteger(code, 0, std::numeric_limits<int>::max(), result_code) ||
        result_code != 0) {
        return {.error = "pending响应业务状态失败"};
    }
    const cJSON* data = cJSON_GetObjectItemCaseSensitive(root.get(), "data");
    if (!HasExactFields(data, {"pending", "event_id", "topic", "priority", "created_at",
                               "expires_at", "retry_after_seconds"})) {
        return {.error = "pending安全信封字段无效"};
    }
    const cJSON* pending = cJSON_GetObjectItemCaseSensitive(data, "pending");
    const cJSON* retry = cJSON_GetObjectItemCaseSensitive(data, "retry_after_seconds");
    int retry_after_seconds = 0;
    if (!cJSON_IsBool(pending) ||
        !ParseInteger(retry, 0, ProbeSchedule::kMaximumFailureDelaySeconds,
                      retry_after_seconds)) {
        return {.error = "pending状态或下次探测时间无效"};
    }
    if (!cJSON_IsTrue(pending)) {
        if (retry_after_seconds < 1) {
            return {.error = "空pending的下次探测时间无效"};
        }
        for (const char* name : {"event_id", "topic", "priority", "created_at", "expires_at"}) {
            if (!cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(data, name))) {
                return {.error = "空pending信封携带了事件字段"};
            }
        }
        return {.success = true, .retry_after_seconds = retry_after_seconds};
    }

    const cJSON* event_id = cJSON_GetObjectItemCaseSensitive(data, "event_id");
    const cJSON* topic = cJSON_GetObjectItemCaseSensitive(data, "topic");
    const cJSON* priority = cJSON_GetObjectItemCaseSensitive(data, "priority");
    PendingEvent event;
    if (!cJSON_IsString(event_id) || event_id->valuestring == nullptr ||
        event_id->valuestring[0] == '\0' || std::strlen(event_id->valuestring) > 64 ||
        !cJSON_IsString(topic) || !IsSupportedTopic(topic->valuestring) ||
        !cJSON_IsString(priority) || !IsSupportedPriority(priority->valuestring) ||
        !ParseDateTime(cJSON_GetObjectItemCaseSensitive(data, "created_at"), event.created_at) ||
        !ParseDateTime(cJSON_GetObjectItemCaseSensitive(data, "expires_at"), event.expires_at) ||
        event.expires_at <= event.created_at) {
        return {.error = "pending事件字段无效"};
    }
    event.event_id = event_id->valuestring;
    event.topic = topic->valuestring;
    event.priority = priority->valuestring;
    return {.success = true, .retry_after_seconds = retry_after_seconds,
            .event = std::move(event)};
}

}  // namespace

ProbeResult ProbePendingEvent() {
    const std::string base_url = GetManagerApiBaseUrl();
    if (base_url.empty()) return {.error = "manager-api地址未配置"};
    auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
    if (http == nullptr) return {.error = "无法创建pending HTTP客户端"};
    http->SetTimeout(10000);
    SetDeviceApiHeaders(*http);
    const std::string path = "/device/proactive/pending";
    if (!http->Open("GET", base_url + path)) {
        ESP_LOGE(TAG, "Pending probe transport error=0x%x", http->GetLastError());
        return {.error = "pending探测连接失败"};
    }
    const int status_code = http->GetStatusCode();
    if (status_code != 200) {
        http->Close();
        ESP_LOGE(TAG, "Pending probe HTTP %d", status_code);
        return {.error = "pending探测HTTP " + std::to_string(status_code)};
    }
    const size_t content_length = http->GetBodyLength();
    if (content_length == 0 || content_length > kMaximumPendingResponseBytes) {
        http->Close();
        ESP_LOGE(TAG, "Pending probe invalid response length=%u",
                 static_cast<unsigned>(content_length));
        return {.error = "pending响应大小无效"};
    }
    std::string body(content_length, '\0');
    size_t total_read = 0;
    while (total_read < content_length) {
        const int read = http->Read(body.data() + total_read, content_length - total_read);
        if (read <= 0 || static_cast<size_t>(read) > content_length - total_read) {
            http->Close();
            return {.error = "pending响应读取不完整"};
        }
        total_read += static_cast<size_t>(read);
    }
    http->Close();
    return ParseResponse(body);
}

}  // namespace external_monitor
