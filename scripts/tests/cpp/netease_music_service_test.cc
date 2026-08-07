#include "netease_music_service.h"

#include <cassert>
#include <deque>
#include <string>

using namespace netease_music;

class FakeClient final : public Client {
public:
    Result<LoginStatus> login = Result<LoginStatus>::Success(LoginStatus::kLoggedOut);
    Result<LoginSession> session = Result<LoginSession>::Success(
        {"short-session", "https://example.invalid/qr.png", 10000, 500});
    Result<LogoutStatus> logout = Result<LogoutStatus>::Success(LogoutStatus::kLoggedOut);
    std::deque<Result<AuthorizationStatus>> authorization;
    int status_calls = 0;
    int create_calls = 0;
    int poll_calls = 0;
    int logout_calls = 0;

    Result<LoginStatus> GetLoginStatus() override {
        ++status_calls;
        return login;
    }
    Result<LoginSession> CreateLoginSession() override {
        ++create_calls;
        return session;
    }
    Result<AuthorizationStatus> GetAuthorizationStatus(const std::string& id) override {
        ++poll_calls;
        assert(id == "short-session");
        assert(!authorization.empty());
        auto result = authorization.front();
        authorization.pop_front();
        return result;
    }
    Result<LogoutStatus> Logout() override {
        ++logout_calls;
        return logout;
    }
};

class FakeScheduler final : public Scheduler {
public:
    uint64_t now = 0;
    std::function<void()> callback;
    uint32_t delay = 0;

    void Schedule(uint32_t value, std::function<void()> next) override {
        delay = value;
        callback = std::move(next);
    }
    void Cancel() override { callback = {}; }
    uint64_t NowMs() const override { return now; }
    void Run() {
        assert(callback);
        auto current = std::move(callback);
        current();
    }
};

class FakeObserver final : public Observer {
public:
    bool can_show = true;
    bool visible = false;
    int show_count = 0;
    int clear_count = 0;
    std::string message;
    std::string qr_status;
    std::string voice;

    bool ShowQrImage(const std::string&) override {
        ++show_count;
        visible = can_show;
        return can_show;
    }
    void UpdateQrStatus(const std::string& value) override { qr_status = value; }
    void ClearQrImage() override {
        ++clear_count;
        visible = false;
    }
    void ShowMessage(const std::string& value) override { message = value; }
    void RequestVoicePrompt(const std::string& value) override { voice = value; }
};

void TestAlreadyLoggedIn() {
    FakeClient client;
    FakeScheduler scheduler;
    FakeObserver observer;
    client.login = Result<LoginStatus>::Success(LoginStatus::kLoggedIn);
    Service service(client, scheduler, observer);
    assert(service.StartLogin().find("已登录") != std::string::npos);
    assert(client.status_calls == 1 && client.create_calls == 0 && !observer.visible);
}

void TestQrPersistsThroughPendingAndNetworkFailureUntilSuccess() {
    FakeClient client;
    FakeScheduler scheduler;
    FakeObserver observer;
    client.authorization.push_back(
        Result<AuthorizationStatus>::Success(AuthorizationStatus::kPending));
    client.authorization.push_back(Result<AuthorizationStatus>::Failure("offline"));
    client.authorization.push_back(
        Result<AuthorizationStatus>::Success(AuthorizationStatus::kAuthorized));
    Service service(client, scheduler, observer);

    assert(service.StartLogin().find("扫描") != std::string::npos);
    assert(observer.visible && scheduler.callback);
    const int status_calls = client.status_calls;
    assert(service.StartLogin().find("已显示") != std::string::npos);
    assert(client.status_calls == status_calls && client.create_calls == 1);

    scheduler.Run();
    assert(observer.visible && scheduler.callback);
    scheduler.Run();
    assert(observer.visible && scheduler.callback);
    assert(observer.qr_status.find("仍然有效") != std::string::npos);
    scheduler.Run();
    assert(!observer.visible && !scheduler.callback);
    assert(service.state() == FlowState::kLoggedIn);
    assert(observer.voice.find("登录成功") != std::string::npos);
}

void TestTerminalSessionStatesCloseQr() {
    for (auto terminal : {AuthorizationStatus::kCancelled, AuthorizationStatus::kExpired,
                          AuthorizationStatus::kFailed}) {
        FakeClient client;
        FakeScheduler scheduler;
        FakeObserver observer;
        client.authorization.push_back(Result<AuthorizationStatus>::Success(terminal));
        Service service(client, scheduler, observer);
        service.StartLogin();
        assert(observer.visible);
        scheduler.Run();
        assert(!observer.visible && !scheduler.callback && !observer.voice.empty());
    }
}

void TestLocalExpiryClosesQr() {
    FakeClient client;
    FakeScheduler scheduler;
    FakeObserver observer;
    Service service(client, scheduler, observer);
    service.StartLogin();
    scheduler.now = 10000;
    scheduler.Run();
    assert(!observer.visible);
    assert(observer.voice.find("过期") != std::string::npos);
}

void TestLogoutOutcomesAndFailedLogoutKeepsActiveSession() {
    {
        FakeClient client;
        FakeScheduler scheduler;
        FakeObserver observer;
        Service service(client, scheduler, observer);
        service.StartLogin();
        assert(service.Logout().find("已退出") != std::string::npos);
        assert(!observer.visible && client.logout_calls == 1);
    }
    {
        FakeClient client;
        FakeScheduler scheduler;
        FakeObserver observer;
        client.logout = Result<LogoutStatus>::Success(LogoutStatus::kAlreadyLoggedOut);
        Service service(client, scheduler, observer);
        assert(service.Logout().find("未登录") != std::string::npos);
    }
    {
        FakeClient client;
        FakeScheduler scheduler;
        FakeObserver observer;
        client.logout = Result<LogoutStatus>::Failure("offline");
        Service service(client, scheduler, observer);
        service.StartLogin();
        assert(service.Logout().find("退出失败") != std::string::npos);
        assert(observer.visible && scheduler.callback);
    }
}

void TestNetworkAndDisplayErrorsAreRetryable() {
    FakeClient client;
    FakeScheduler scheduler;
    FakeObserver observer;
    client.login = Result<LoginStatus>::Failure("offline");
    Service service(client, scheduler, observer);
    assert(service.StartLogin().find("检查网络") != std::string::npos);
    assert(service.state() == FlowState::kRetryableError);

    client.login = Result<LoginStatus>::Success(LoginStatus::kLoggedOut);
    observer.can_show = false;
    assert(service.StartLogin().find("无法显示") != std::string::npos);
    assert(service.state() == FlowState::kRetryableError);
}

int main() {
    TestAlreadyLoggedIn();
    TestQrPersistsThroughPendingAndNetworkFailureUntilSuccess();
    TestTerminalSessionStatesCloseQr();
    TestLocalExpiryClosesQr();
    TestLogoutOutcomesAndFailedLogoutKeepsActiveSession();
    TestNetworkAndDisplayErrorsAreRetryable();
    return 0;
}
