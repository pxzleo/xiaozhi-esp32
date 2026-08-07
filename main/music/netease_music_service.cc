#include "netease_music_service.h"

#include <algorithm>

namespace netease_music {
namespace {

constexpr uint32_t kDefaultPollIntervalMs = 2000;
constexpr uint32_t kMinimumPollIntervalMs = 500;
constexpr uint32_t kMaximumPollIntervalMs = 10000;

}  // namespace

Service::Service(Client& client, Scheduler& scheduler, Observer& observer)
    : client_(client), scheduler_(scheduler), observer_(observer) {}

Service::~Service() { scheduler_.Cancel(); }

std::string Service::StartLogin() {
    if (state_ == FlowState::kWaitingForAuthorization) {
        const std::string message = "网易云音乐登录二维码已显示，请扫码确认。";
        observer_.ShowMessage(message);
        return message;
    }

    ++generation_;
    scheduler_.Cancel();
    observer_.ClearQrImage();

    auto login_status = client_.GetLoginStatus();
    if (!login_status.ok) {
        state_ = FlowState::kRetryableError;
        const std::string message = "暂时无法查询网易云音乐登录状态，请检查网络后重试。";
        observer_.ShowMessage(message);
        return message;
    }
    if (login_status.value == LoginStatus::kLoggedIn) {
        state_ = FlowState::kLoggedIn;
        const std::string message = "网易云音乐当前已登录，无需重复扫码。";
        observer_.ShowMessage(message);
        return message;
    }

    auto created = client_.CreateLoginSession();
    if (!created.ok || created.value.session_id.empty() || created.value.qr_image_url.empty() ||
        created.value.expires_in_ms == 0) {
        state_ = FlowState::kRetryableError;
        const std::string message = "暂时无法创建网易云音乐登录二维码，请稍后重试。";
        observer_.ShowMessage(message);
        return message;
    }

    session_ = std::move(created.value);
    if (!observer_.ShowQrImage(session_.qr_image_url)) {
        session_ = {};
        state_ = FlowState::kRetryableError;
        const std::string message = "当前设备无法显示登录二维码，请稍后重试。";
        observer_.ShowMessage(message);
        return message;
    }

    state_ = FlowState::kWaitingForAuthorization;
    consecutive_poll_failures_ = 0;
    expires_at_ms_ = scheduler_.NowMs() + session_.expires_in_ms;
    const std::string message = "请使用网易云音乐扫描屏幕上的二维码并确认登录。";
    observer_.ShowMessage(message);
    SchedulePoll();
    return message;
}

std::string Service::Logout() {
    const bool was_waiting = state_ == FlowState::kWaitingForAuthorization;
    ++generation_;
    scheduler_.Cancel();

    auto result = client_.Logout();
    if (!result.ok) {
        if (was_waiting) {
            state_ = FlowState::kWaitingForAuthorization;
            observer_.UpdateQrStatus("退出请求失败，当前登录二维码仍然有效，正在继续查询。" );
            SchedulePoll();
        } else {
            state_ = FlowState::kRetryableError;
        }
        const std::string message = "网易云音乐退出失败，请检查网络后重试。";
        observer_.ShowMessage(message);
        return message;
    }

    observer_.ClearQrImage();
    session_ = {};
    expires_at_ms_ = 0;
    state_ = FlowState::kIdle;
    if (result.value == LogoutStatus::kAlreadyLoggedOut) {
        const std::string message = "网易云音乐当前未登录。";
        observer_.ShowMessage(message);
        return message;
    }

    const std::string message = "已退出网易云音乐。";
    observer_.ShowMessage(message);
    return message;
}

void Service::SchedulePoll() {
    uint32_t interval = session_.poll_interval_ms == 0 ? kDefaultPollIntervalMs
                                                       : session_.poll_interval_ms;
    interval = std::clamp(interval, kMinimumPollIntervalMs, kMaximumPollIntervalMs);
    const uint32_t generation = generation_;
    scheduler_.Schedule(interval, [this, generation]() { Poll(generation); });
}

void Service::Poll(uint32_t generation) {
    if (generation != generation_ || state_ != FlowState::kWaitingForAuthorization) {
        return;
    }
    if (scheduler_.NowMs() >= expires_at_ms_) {
        Finish(FlowState::kRetryableError, "网易云音乐登录二维码已过期，请重新发起登录。" );
        return;
    }

    auto status = client_.GetAuthorizationStatus(session_.session_id);
    if (!status.ok) {
        ++consecutive_poll_failures_;
        const std::string message = "网络暂时不可用，登录二维码仍然有效，正在重试。";
        observer_.UpdateQrStatus(message);
        observer_.ShowMessage(message);
        SchedulePoll();
        return;
    }
    const bool network_recovered = consecutive_poll_failures_ > 0;
    consecutive_poll_failures_ = 0;

    switch (status.value) {
        case AuthorizationStatus::kPending:
            if (network_recovered) {
                observer_.UpdateQrStatus("网络已恢复，请继续扫码并确认。" );
            }
            SchedulePoll();
            return;
        case AuthorizationStatus::kAuthorized:
            Finish(FlowState::kLoggedIn, "网易云音乐登录成功。" );
            return;
        case AuthorizationStatus::kExpired:
            Finish(FlowState::kRetryableError,
                   "网易云音乐登录二维码已过期，请重新发起登录。" );
            return;
        case AuthorizationStatus::kCancelled:
            Finish(FlowState::kIdle, "已取消网易云音乐登录，可以随时重新发起。" );
            return;
        case AuthorizationStatus::kFailed:
            Finish(FlowState::kRetryableError, "网易云音乐授权失败，请重新发起登录。" );
            return;
    }
}

void Service::Finish(FlowState state, const std::string& message) {
    ++generation_;
    scheduler_.Cancel();
    observer_.ClearQrImage();
    session_ = {};
    expires_at_ms_ = 0;
    state_ = state;
    observer_.ShowMessage(message);
    observer_.RequestVoicePrompt(message);
}

}  // namespace netease_music
