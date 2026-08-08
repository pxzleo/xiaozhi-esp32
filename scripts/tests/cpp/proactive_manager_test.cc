#include "proactive_manager.h"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <stdexcept>

using namespace proactive;

std::time_t At(int year, int month, int day, int hour, int minute = 0) {
    std::tm value{};
    value.tm_year = year - 1900;
    value.tm_mon = month - 1;
    value.tm_mday = day;
    value.tm_hour = hour;
    value.tm_min = minute;
    value.tm_isdst = -1;
    return std::mktime(&value);
}

Event Suggestion(std::string id, std::string topic, std::time_t now) {
    return {std::move(id), std::move(topic), Priority::kNormal, "test", now,
            now + 600, "suggestion", false, {}};
}

void TestModesBudgetDateAndTimeValidity() {
    Manager manager;
    assert(manager.config().mode == Mode::kAggressive);
    assert(manager.config().daily_limit == 5);
    const auto day = At(2026, 8, 8, 9);
    assert(!manager.ShouldDeliver(Suggestion("0", "weather", day), day, false));
    for (int i = 0; i < 5; ++i) {
        auto event = Suggestion(std::to_string(i + 1), "topic" + std::to_string(i), day + i);
        assert(manager.ShouldDeliver(event, day + i, true));
        manager.RecordDelivered(event, day + i, true);
    }
    assert(!manager.ShouldDeliver(Suggestion("6", "sixth", day + 10), day + 10, true));
    assert(manager.ShouldDeliver(Suggestion("7", "next", At(2026, 8, 9, 9)),
                                 At(2026, 8, 9, 9), true));

    manager.Configure(Mode::kActive, std::nullopt, std::nullopt, std::nullopt);
    assert(manager.config().daily_limit == 3);
    manager.Configure(Mode::kConservative, std::nullopt, std::nullopt, std::nullopt);
    assert(!manager.ShouldDeliver(Suggestion("8", "weather", At(2026, 8, 10, 9)),
                                  At(2026, 8, 10, 9), true));
    Event critical{"9", "health", Priority::kCritical, "critical health", day,
                   day + 60, "health-critical", false, {}};
    assert(manager.ShouldDeliver(critical, day, false));
}

void TestQuietMuteTopicAndCooldown() {
    Manager manager;
    const auto day = At(2026, 8, 8, 22, 30);
    manager.Configure(Mode::kAggressive, 5, 22 * 60, 7 * 60);
    assert(!manager.ShouldDeliver(Suggestion("1", "weather", day), day, true));
    assert(manager.ShouldDeliver(Suggestion("2", "weather", At(2026, 8, 9, 8)),
                                 At(2026, 8, 9, 8), true));
    manager.BlockTopic("weather");
    assert(!manager.ShouldDeliver(Suggestion("3", "weather", At(2026, 8, 9, 9)),
                                  At(2026, 8, 9, 9), true));
    manager.AllowTopic("weather");
    assert(!manager.ShouldDeliver(Suggestion("whitelist", "other", At(2026, 8, 9, 9)),
                                  At(2026, 8, 9, 9), true));
    Event whitelist_critical{"critical", "other", Priority::kCritical, "critical",
                             At(2026, 8, 9, 9), At(2026, 8, 9, 10),
                             "critical-other", false, {}};
    assert(manager.ShouldDeliver(whitelist_critical, At(2026, 8, 9, 9), true));
    auto first = Suggestion("4", "weather", At(2026, 8, 9, 9));
    assert(manager.ShouldDeliver(first, first.created_at, true));
    manager.RecordDelivered(first, first.created_at, true);
    assert(!manager.ShouldDeliver(Suggestion("5", "weather", first.created_at + 60),
                                  first.created_at + 60, true));
    manager.AllowTopic("other");

    manager.MuteToday(At(2026, 8, 9, 10), true);
    assert(manager.config().mode == Mode::kTodaySilent);
    assert(!manager.ShouldDeliver(Suggestion("6", "other", At(2026, 8, 9, 11)),
                                  At(2026, 8, 9, 11), true));
    assert(manager.ShouldDeliver(Suggestion("7", "other", At(2026, 8, 10, 8)),
                                 At(2026, 8, 10, 8), true));
    assert(manager.config().mode == Mode::kAggressive);
}

void TestFollowUpLifecycleAndRecovery() {
    FollowUpStore store;
    const auto now = At(2026, 8, 8, 9);
    store.Schedule(42, "吃药", now - 30, now);
    assert(store.items()[0].source_triggered_at == now - 30);
    assert(store.items()[0].due_at == now + 600);
    assert(store.PendingDue(now + 599).empty());
    auto due = store.PendingDue(now + 600);
    assert(due.size() == 1 && due[0].source_id == 42 && !due[0].asked);
    store.MarkAsked(due[0].source_id, due[0].due_at);
    assert(store.PendingDue(now + 601).empty());  // 最多追问一次。
    store.DelayRecent(15, now + 601);
    assert(store.PendingDue(now + 1500).empty());
    assert(store.PendingDue(now + 1501).size() == 1);
    assert(store.CompleteRecent(now + 1502).source_id == 42);
    assert(store.items().empty());

    store.Schedule(7, "关窗", now, now);
    FollowUpStore restored;
    restored.Restore(store.items());
    assert(restored.PendingDue(now + 600).size() == 1);
    FollowUpStore delayed_connection;
    delayed_connection.Schedule(70, "两分钟后通道恢复", now, now);
    auto delayed_due = delayed_connection.PendingDue(now + 720);
    assert(delayed_due.size() == 1 && delayed_due[0].expires_at == now + 900);
    delayed_connection.MarkAsked(delayed_due[0].source_id, delayed_due[0].due_at);
    assert(delayed_connection.items()[0].asked);
    store.Schedule(8, "喝水", now + 1, now + 1);
    assert(store.DismissRecent(now + 2).source_id == 8);
    assert(store.DismissRecent(now + 2).source_id == 7);
    bool no_recent = false;
    try { store.DismissRecent(now + 2); } catch (const std::runtime_error&) { no_recent = true; }
    assert(no_recent);

    FollowUpStore ambiguous;
    ambiguous.Schedule(10, "甲", now, now + 10);
    ambiguous.Schedule(11, "乙", now, now + 20);
    bool ambiguity_reported = false;
    try { ambiguous.CompleteRecent(now + 1); }
    catch (const std::runtime_error&) { ambiguity_reported = true; }
    assert(ambiguity_reported && ambiguous.items().size() == 2);

    FollowUpStore expired;
    expired.Schedule(9, "过期", now, now);
    assert(expired.PendingDue(now + 901).empty());
    assert(expired.items().empty());
}

void TestHealthDedupRecoveryAndQueueOrdering() {
    HealthTracker health;
    const auto now = At(2026, 8, 8, 9);
    auto raised = health.Raise("network_flapping", Severity::kWarning, now,
                               {{"disconnects", "3"}});
    assert(raised && raised->event.topic == "health" &&
           raised->event.priority == Priority::kHigh);
    auto info = health.Raise("ota_update_available", Severity::kInfo, now, {});
    assert(info && info->event.topic == "health" &&
           info->event.priority == Priority::kNormal);
    auto critical_health = health.Raise("audio_decode_failed", Severity::kCritical, now, {});
    assert(critical_health && critical_health->event.topic == "health_critical" &&
           critical_health->event.priority == Priority::kCritical);
    assert(!health.Raise("network_flapping", Severity::kWarning, now + 1, {}));
    auto recovered = health.Recover("network_flapping", now + 2);
    assert(recovered && recovered->event.dedupe_key == raised->event.dedupe_key);
    assert(!health.Recover("network_flapping", now + 3));
    assert(!health.Raise("network_flapping", Severity::kWarning, now + 4, {}));
    assert(health.Raise("network_flapping", Severity::kWarning, now + 1803, {}));

    DurableQueue queue;
    queue.Push(Suggestion("normal", "tip", now));
    Event alarm{"alarm", "alarm", Priority::kCritical, "alarm", now + 1,
                now + 60, "alarm-1", true, {}};
    queue.Push(alarm);
    assert(queue.PopNext(now + 1)->event_id == "alarm");
    Event follow_up = Suggestion("follow-up", "follow_up", now);
    follow_up.dedupe_key = "follow-up:42";
    queue.Push(follow_up);
    assert(queue.RemoveByDedupeKey("follow-up:42") == 1);
    queue.Push(Suggestion("expired", "tip", now - 1000));
    assert(queue.DropExpired(now + 1) == 1);
}

void TestRetryBackoffIsBounded() {
    RetryBackoff backoff;
    assert(backoff.Ready(0));
    backoff.OnFailure(1000000);
    assert(!backoff.Ready(5999999));
    assert(backoff.Ready(6000000));
    for (int i = 0; i < 10; ++i) backoff.OnFailure(backoff.retry_after_us());
    assert(backoff.delay_seconds() == 300);
    backoff.OnSuccess();
    assert(backoff.delay_seconds() == 5 && backoff.Ready(0));
}

void TestPersistentCapacityLimits() {
    const auto now = At(2026, 8, 8, 9);
    FollowUpStore follow_ups;
    for (uint32_t id = 1; id <= FollowUpStore::kMaxItems; ++id) {
        follow_ups.Schedule(id, "容量", now + id, now + id);
    }
    bool follow_up_full = false;
    try { follow_ups.Schedule(99, "超限", now + 99, now + 99); }
    catch (const std::runtime_error&) { follow_up_full = true; }
    assert(follow_up_full);

    DurableQueue queue;
    for (size_t i = 0; i < DurableQueue::kMaxItems; ++i) {
        auto event = Suggestion("capacity-" + std::to_string(i),
                                "topic-" + std::to_string(i), now);
        event.dedupe_key = event.event_id;
        queue.Push(std::move(event));
    }
    bool queue_full = false;
    try {
        auto event = Suggestion("overflow", "overflow", now);
        event.dedupe_key = event.event_id;
        queue.Push(std::move(event));
    } catch (const std::runtime_error&) { queue_full = true; }
    assert(queue_full);

    DurableQueue priority_queue;
    for (size_t i = 0; i < DurableQueue::kMaxItems; ++i) {
        auto low = Suggestion("low-" + std::to_string(i), "low", now);
        low.priority = Priority::kLow;
        low.dedupe_key = low.event_id;
        priority_queue.Push(std::move(low));
    }
    Event critical{"critical-capacity", "health_critical", Priority::kCritical,
                   "critical", now, now + 60, "critical-capacity", false, {}};
    auto evicted = priority_queue.Push(critical);
    assert(evicted && evicted->priority == Priority::kLow);
    assert(priority_queue.items().size() == DurableQueue::kMaxItems);
    Event ota_info{"ota-info", "health", Priority::kNormal,
                   "ota info health", now, now + 60, "health:ota", false,
                   {{"health_kind", "ota_update_available"}, {"severity", "info"}}};
    auto ota_evicted = priority_queue.Push(ota_info);
    assert(ota_evicted && ota_evicted->priority == Priority::kLow);
    assert(std::any_of(priority_queue.items().begin(), priority_queue.items().end(),
                       [](const Event& item) { return item.event_id == "ota-info"; }));
    DurableQueue same_priority_queue;
    for (size_t i = 0; i < DurableQueue::kMaxItems; ++i) {
        auto ordinary = Suggestion("normal-" + std::to_string(i), "normal", now);
        ordinary.priority = Priority::kNormal;
        ordinary.dedupe_key = ordinary.event_id;
        same_priority_queue.Push(std::move(ordinary));
    }
    auto ordinary_evicted = same_priority_queue.Push(ota_info);
    assert(ordinary_evicted && ordinary_evicted->metadata.count("health_kind") == 0);
    assert(std::any_of(same_priority_queue.items().begin(), same_priority_queue.items().end(),
                       [](const Event& item) { return item.event_id == "ota-info"; }));

    DurableQueue protected_queue;
    for (size_t i = 0; i < DurableQueue::kMaxItems; ++i) {
        Event protected_event{"protected-" + std::to_string(i), "health_critical",
            Priority::kCritical, "protected", now, now + 60,
            "protected-" + std::to_string(i), false, {}};
        if (i == 0) protected_event.metadata["recovered"] = "true";
        protected_queue.Push(std::move(protected_event));
    }
    bool ordinary_rejected = false;
    try {
        auto ordinary = Suggestion("ordinary", "ordinary", now);
        ordinary.dedupe_key = ordinary.event_id;
        protected_queue.Push(std::move(ordinary));
    } catch (const std::runtime_error&) { ordinary_rejected = true; }
    assert(ordinary_rejected && protected_queue.items().size() == DurableQueue::kMaxItems);
    std::optional<Event> pending_ota;
    try {
        protected_queue.Push(ota_info);
    } catch (const std::runtime_error&) {
        pending_ota = ota_info;
    }
    assert(pending_ota && pending_ota->metadata.at("health_kind") ==
                              "ota_update_available");

    FollowUpStore transactional;
    transactional.Schedule(200, "事务追问", now, now);
    const auto pending = transactional.PendingDue(now + 600);
    assert(pending.size() == 1 && !transactional.items()[0].asked);
    bool enqueue_failed = false;
    try {
        auto follow_up = Suggestion("follow-up-full", "follow_up", now);
        follow_up.priority = Priority::kLow;
        follow_up.dedupe_key = "follow-up:200";
        priority_queue.Push(std::move(follow_up));
    } catch (const std::runtime_error&) { enqueue_failed = true; }
    assert(enqueue_failed && !transactional.items()[0].asked);
}

void TestTopicRuleMigrationAtCapacity() {
    Manager manager;
    manager.BlockTopic("move-me");
    for (int i = 0; i < 7; ++i) manager.BlockTopic("blocked-" + std::to_string(i));
    assert(manager.config().blocked_topics.size() == 8);
    manager.AllowTopic("move-me");
    assert(manager.config().blocked_topics.size() == 7);
    assert(manager.config().allowed_topics.count("move-me") == 1);
    manager.BlockTopic("move-me");
    assert(manager.config().blocked_topics.size() == 8);
    assert(manager.config().allowed_topics.empty());
}

int main() {
    setenv("TZ", "UTC", 1);
    tzset();
    TestModesBudgetDateAndTimeValidity();
    TestQuietMuteTopicAndCooldown();
    TestFollowUpLifecycleAndRecovery();
    TestHealthDedupRecoveryAndQueueOrdering();
    TestRetryBackoffIsBounded();
    TestPersistentCapacityLimits();
    TestTopicRuleMigrationAtCapacity();
    return 0;
}
