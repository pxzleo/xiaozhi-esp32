#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>

namespace netease_music {

enum class LoginStatus { kLoggedOut, kLoggedIn };
enum class AuthorizationStatus { kPending, kAuthorized, kExpired, kCancelled, kFailed };
enum class LogoutStatus { kLoggedOut, kAlreadyLoggedOut };
enum class FlowState { kIdle, kWaitingForAuthorization, kLoggedIn, kRetryableError };

template <typename T>
struct Result {
    bool ok = false;
    T value{};
    std::string error;

    static Result Success(T value) { return {true, std::move(value), {}}; }
    static Result Failure(std::string error) { return {false, {}, std::move(error)}; }
};

struct LoginSession {
    // An opaque, short-lived identifier. It must not contain a cookie, account password,
    // access token, or any other long-lived credential.
    std::string session_id;
    std::string qr_image_url;
    uint32_t expires_in_ms = 0;
    uint32_t poll_interval_ms = 0;
};

class Client {
public:
    virtual ~Client() = default;
    virtual Result<LoginStatus> GetLoginStatus() = 0;
    virtual Result<LoginSession> CreateLoginSession() = 0;
    virtual Result<AuthorizationStatus> GetAuthorizationStatus(
        const std::string& session_id) = 0;
    virtual Result<LogoutStatus> Logout() = 0;
};

class Scheduler {
public:
    virtual ~Scheduler() = default;
    virtual void Schedule(uint32_t delay_ms, std::function<void()> callback) = 0;
    virtual void Cancel() = 0;
    virtual uint64_t NowMs() const = 0;
};

class Observer {
public:
    virtual ~Observer() = default;
    virtual bool ShowQrImage(const std::string& image_url) = 0;
    virtual void UpdateQrStatus(const std::string& message) = 0;
    virtual void ClearQrImage() = 0;
    virtual void ShowMessage(const std::string& message) = 0;
    // Terminal authorization changes happen after the original MCP call has returned.
    // The platform adapter forwards these prompts to the conversation server for TTS.
    virtual void RequestVoicePrompt(const std::string& message) = 0;
};

class Service {
public:
    Service(Client& client, Scheduler& scheduler, Observer& observer);
    ~Service();

    // Return strings are deliberately user-facing: the MCP caller should speak them verbatim.
    std::string StartLogin();
    std::string Logout();
    FlowState state() const { return state_; }

private:
    void SchedulePoll();
    void Poll(uint32_t generation);
    void Finish(FlowState state, const std::string& message);

    Client& client_;
    Scheduler& scheduler_;
    Observer& observer_;
    FlowState state_ = FlowState::kIdle;
    LoginSession session_;
    uint64_t expires_at_ms_ = 0;
    uint32_t generation_ = 0;
    uint32_t consecutive_poll_failures_ = 0;
};

}  // namespace netease_music
