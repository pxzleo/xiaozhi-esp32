#include "schedule_manager.h"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <stdexcept>

using namespace schedule;

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

Task Add(Manager& manager, Kind kind, Repeat repeat, std::time_t when,
         std::vector<int> weekdays = {}) {
    CreateRequest request;
    request.kind = kind;
    request.repeat = repeat;
    request.label = "测试提醒";
    request.trigger_at = when;
    request.weekdays = std::move(weekdays);
    return manager.Create(request, when - 100);
}

void TestLimitDeleteAndClear() {
    Manager manager;
    for (size_t i = 0; i < Manager::kMaxTasks; ++i) {
        Add(manager, Kind::kReminder, Repeat::kOnce, At(2026, 8, 8, 8) + i);
    }
    bool rejected = false;
    try {
        Add(manager, Kind::kAlarm, Repeat::kOnce, At(2026, 8, 9, 8));
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    assert(rejected);
    assert(manager.Delete(3));
    assert(!manager.Delete(999));
    assert(manager.Clear() == 15 && manager.tasks().empty());
}

void TestFiveRepeatsAndCrossDay() {
    Manager manager;
    const auto friday = At(2026, 8, 7, 9);
    const auto saturday = At(2026, 8, 8, 9);
    auto once = Add(manager, Kind::kReminder, Repeat::kOnce, friday);
    auto daily = Add(manager, Kind::kReminder, Repeat::kDaily, friday);
    auto weekdays = Add(manager, Kind::kReminder, Repeat::kWeekdays, friday);
    auto weekends = Add(manager, Kind::kReminder, Repeat::kWeekends, saturday);
    auto weekly = Add(manager, Kind::kReminder, Repeat::kWeekly, friday, {1, 3, 5});
    assert(Manager::NextOccurrence(once, friday) == 0);
    assert(Manager::NextOccurrence(daily, friday) == At(2026, 8, 8, 9));
    assert(Manager::NextOccurrence(weekdays, friday) == At(2026, 8, 10, 9));
    assert(Manager::NextOccurrence(weekends, saturday) == At(2026, 8, 9, 9));
    assert(Manager::NextOccurrence(weekly, friday) == At(2026, 8, 10, 9));
}

void TestKindFiltersAndScopedClear() {
    Manager manager;
    Add(manager, Kind::kAlarm, Repeat::kOnce, At(2026, 8, 8, 8));
    Add(manager, Kind::kReminder, Repeat::kOnce, At(2026, 8, 8, 9));
    Add(manager, Kind::kAlarm, Repeat::kOnce, At(2026, 8, 8, 10));
    assert(manager.List(KindFilter::kAll).size() == 3);
    assert(manager.List(KindFilter::kAlarm).size() == 2);
    assert(manager.List(KindFilter::kReminder).size() == 1);
    assert(manager.Clear(KindFilter::kAlarm) == 2);
    assert(manager.List(KindFilter::kAll).size() == 1);
    assert(manager.Clear(KindFilter::kReminder) == 1);
    assert(manager.Clear(KindFilter::kAll) == 0);
    assert(Manager::ParseKindFilter("all") == KindFilter::kAll);
}

void TestFirstRepeatConstraintAndFirstTick() {
    const auto friday = At(2026, 8, 7, 9);
    const auto saturday = At(2026, 8, 8, 9);
    Manager manager;
    auto expect_rejected = [&manager](Repeat repeat, std::time_t when,
                                      std::vector<int> weekdays = {}) {
        bool rejected = false;
        try {
            Add(manager, Kind::kReminder, repeat, when, std::move(weekdays));
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        assert(rejected);
    };
    expect_rejected(Repeat::kWeekdays, saturday);
    expect_rejected(Repeat::kWeekends, friday);
    expect_rejected(Repeat::kWeekly, friday, {1, 3});

    manager.Tick(friday - 1, true);
    auto task = Add(manager, Kind::kAlarm, Repeat::kWeekdays, friday);
    auto first = manager.Tick(friday, true);
    assert(first.triggered.size() == 1 && first.triggered[0].id == task.id);
    assert(manager.tasks().size() == 1);
    assert(manager.tasks()[0].trigger_at == At(2026, 8, 10, 9));
}

void TestNaturalTaskDescription() {
    Manager manager;
    auto task = Add(manager, Kind::kAlarm, Repeat::kWeekly, At(2026, 8, 7, 9), {1, 5});
    const auto response = Manager::DescribeTask(task);
    assert(response.find("任务ID 1") != std::string::npos);
    assert(response.find("类型闹铃") != std::string::npos);
    assert(response.find("2026-08-07T09:00:00") != std::string::npos);
    assert(response.find("重复规则每周1、5") != std::string::npos);
    assert(response.find("内容“测试提醒”") != std::string::npos);
}

void TestShortCreationDescription() {
    Manager manager;
    const auto now = At(2026, 8, 7, 20);
    auto today = Add(manager, Kind::kAlarm, Repeat::kOnce, At(2026, 8, 7, 21));
    auto tomorrow = Add(manager, Kind::kReminder, Repeat::kOnce, At(2026, 8, 8, 8, 30));
    auto day_after = Add(manager, Kind::kAlarm, Repeat::kOnce, At(2026, 8, 9, 9));
    assert(Manager::DescribeCreation(today, now) == "已设置今天21点的闹铃。");
    assert(Manager::DescribeCreation(tomorrow, now) == "已设置明天8点30分提醒你测试提醒。");
    assert(Manager::DescribeCreation(day_after, now) == "已设置后天9点的闹铃。");
    assert(Manager::DescribeCreation(tomorrow, now).find("任务ID") == std::string::npos);

    Task weekly{9, Kind::kReminder, Repeat::kWeekly, "吃药", At(2026, 8, 7, 9),
                {1, 5}, "", ""};
    assert(Manager::DescribeCreation(weekly, now) == "已设置每周一、五9点提醒你吃药。");
}

void TestTemporaryVolumeRestoreDecision() {
    assert(ShouldRestoreTemporaryVolume(7, 7));
    assert(!ShouldRestoreTemporaryVolume(7, 8));
}

void TestWhitespaceLabels() {
    const auto when = At(2026, 8, 8, 8);
    auto create = [when](const std::string& label) {
        Manager manager;
        CreateRequest request;
        request.kind = Kind::kReminder;
        request.repeat = Repeat::kOnce;
        request.label = label;
        request.trigger_at = when;
        return manager.Create(request, when - 1);
    };
    for (const auto& label : {std::string(" \t\r\n"), std::string("　　")}) {
        bool rejected = false;
        try {
            create(label);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        assert(rejected);
    }
    const std::string padded = "　 喝水 \t";
    assert(create(padded).label == padded);

    Manager restored;
    bool rejected_restore = false;
    try {
        restored.Restore({{1, Kind::kReminder, Repeat::kOnce, "　", when, {}, "", ""}}, 2);
    } catch (const std::invalid_argument&) {
        rejected_restore = true;
    }
    assert(rejected_restore);
}

void TestRecoveryAndOrderingAndDeduplication() {
    const auto now = At(2026, 8, 7, 10);
    Manager recovery;
    recovery.Restore({{1, Kind::kAlarm, Repeat::kOnce, "补响", now - 300, {}, "", ""},
                      {2, Kind::kReminder, Repeat::kOnce, "错过", now - 301, {}, "", ""},
                      {3, Kind::kAlarm, Repeat::kDaily, "重复", At(2026, 1, 1, 9), {}, "", ""}},
                     4);
    auto recovered = recovery.Tick(now, true);
    assert(recovered.triggered.size() == 1 && recovered.triggered[0].id == 1);
    assert(recovered.missed.size() == 1 && recovered.missed[0].id == 2);
    assert(recovery.tasks().size() == 1 && recovery.tasks()[0].trigger_at > now);

    Manager manager;
    manager.Tick(now - 10, true);  // 完成启动恢复阶段。
    auto reminder = Add(manager, Kind::kReminder, Repeat::kOnce, now);
    auto alarm2 = Add(manager, Kind::kAlarm, Repeat::kOnce, now);
    auto alarm1 = Add(manager, Kind::kAlarm, Repeat::kOnce, now);
    CreateRequest briefing_request;
    briefing_request.kind = Kind::kBriefing;
    briefing_request.repeat = Repeat::kOnce;
    briefing_request.label = "简报";
    briefing_request.trigger_at = now;
    briefing_request.sections = "weather,news";
    briefing_request.location = "广州";
    auto briefing = manager.Create(briefing_request, now - 10);
    auto due = manager.Tick(now, true);
    assert(due.triggered.size() == 4);
    assert(due.triggered[0].id == alarm2.id);
    assert(due.triggered[1].id == alarm1.id);
    assert(due.triggered[2].id == reminder.id);
    assert(due.triggered[3].id == briefing.id);
    assert(manager.Tick(now + 1, true).triggered.empty());
}

void TestInvalidTimeStopAndSnooze() {
    const auto now = At(2026, 8, 7, 10);
    Manager manager;
    auto task = Add(manager, Kind::kAlarm, Repeat::kOnce, now);
    assert(manager.Tick(now, false).triggered.empty());
    assert(manager.tasks().size() == 1);

    AlertQueue alerts;
    alerts.Enqueue(task);
    assert(alerts.StartNext()->id == task.id && alerts.active());
    auto stopped = alerts.Stop();
    assert(stopped && stopped->id == task.id && !alerts.active());

    auto snoozed = manager.Snooze(task, 5, now);
    assert(snoozed.repeat == Repeat::kOnce);
    assert(snoozed.trigger_at == now + 300);
    bool rejected = false;
    try {
        manager.Snooze(task, 61, now);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);
}

void TestReminderDeliverySequence() {
    ReminderDeliverySequence sequence;
    assert(sequence.state() == ReminderDeliveryState::kInactive);

    sequence.Begin();
    assert(sequence.state() == ReminderDeliveryState::kWaitingForCue);
    assert(!sequence.NeedsServerAbort());
    assert(sequence.OnPlaybackDrained());
    assert(sequence.state() == ReminderDeliveryState::kWaitingForTts);
    assert(sequence.NeedsServerAbort());
    assert(sequence.CancelWaitingForTts());
    assert(sequence.state() == ReminderDeliveryState::kInactive);
    assert(!sequence.CancelWaitingForTts());

    sequence.Begin();
    assert(sequence.OnPlaybackDrained());
    assert(!sequence.OnPlaybackDrained());
    assert(sequence.OnTtsStarted());
    assert(sequence.state() == ReminderDeliveryState::kSpeaking);
    assert(sequence.NeedsServerAbort());
    assert(sequence.OnTtsStopped());
    assert(sequence.state() == ReminderDeliveryState::kInactive);
    assert(!sequence.NeedsServerAbort());

    // An unrelated or stale TTS stop must not activate listening.
    assert(!sequence.OnTtsStopped());
    sequence.Begin();
    assert(sequence.state() == ReminderDeliveryState::kWaitingForCue);
}

void TestBriefingValidationAndFilter() {
    const auto when = At(2026, 8, 10, 8);
    Manager manager;
    CreateRequest request;
    request.kind = Kind::kBriefing;
    request.repeat = Repeat::kWeekdays;
    request.label = "广州天气和新闻";
    request.trigger_at = when;
    request.sections = "weather,news";
    request.location = "广州";
    const auto briefing = manager.Create(request, when - 60);
    assert(briefing.kind == Kind::kBriefing);
    assert(manager.List(KindFilter::kBriefing).size() == 1);
    assert(Manager::DescribeCreation(briefing, when - 60) ==
           "已设置工作日8点的每日简报。");
    bool snooze_rejected = false;
    try {
        manager.Snooze(briefing, 5, when);
    } catch (const std::invalid_argument&) {
        snooze_rejected = true;
    }
    assert(snooze_rejected);

    request.location.clear();
    bool location_rejected = false;
    try {
        Manager invalid;
        invalid.Create(request, when - 60);
    } catch (const std::invalid_argument&) {
        location_rejected = true;
    }
    assert(location_rejected);
}

void TestRepeatingSuggestionUsesRealTasks() {
    const auto first_time = At(2026, 8, 10, 8);
    Manager manager;
    auto first = Add(manager, Kind::kReminder, Repeat::kOnce, first_time);
    assert(!Manager::ShouldSuggestRepeating(manager.tasks(), first));

    CreateRequest request;
    request.kind = Kind::kReminder;
    request.repeat = Repeat::kOnce;
    request.label = first.label;
    request.trigger_at = At(2026, 8, 11, 8);
    auto second = manager.Create(request, first_time - 60);
    assert(Manager::ShouldSuggestRepeating(manager.tasks(), second));

    request.label = "不同内容";
    request.trigger_at = At(2026, 8, 12, 8);
    auto different = manager.Create(request, first_time - 60);
    assert(!Manager::ShouldSuggestRepeating(manager.tasks(), different));

    Manager same_day_manager;
    auto same_day_base = Add(
        same_day_manager, Kind::kReminder, Repeat::kOnce, first_time);
    request.label = same_day_base.label;
    request.trigger_at = first_time + 30;
    auto same_day_different_second =
        same_day_manager.Create(request, first_time - 60);
    assert(!Manager::ShouldSuggestRepeating(
        same_day_manager.tasks(), same_day_different_second));

    Manager seconds_manager;
    auto seconds_base = Add(
        seconds_manager, Kind::kReminder, Repeat::kOnce, first_time);
    request.label = seconds_base.label;
    request.trigger_at = At(2026, 8, 13, 8) + 30;
    auto different_second = seconds_manager.Create(request, first_time - 60);
    assert(!Manager::ShouldSuggestRepeating(
        seconds_manager.tasks(), different_second));
}

void TestSharedScheduleMetadataAndLegacyMigration() {
    const auto when = At(2026, 8, 14, 8);
    Manager manager;
    auto created = Add(manager, Kind::kReminder, Repeat::kOnce, when);
    assert(created.source_schedule_id == "1");
    assert(created.schedule_uuid.empty());
    assert(created.schedule_version == 0);
    assert(created.sync_state == SyncState::kPending);

    assert(manager.BindAuthority(created.id,
                                 "4d8b9aec-1e54-4d3e-a9f4-c37a7f21c995", 3));
    const auto* bound = manager.Find(created.id);
    assert(bound != nullptr);
    assert(bound->schedule_uuid == "4d8b9aec-1e54-4d3e-a9f4-c37a7f21c995");
    assert(bound->schedule_version == 3);
    assert(bound->sync_state == SyncState::kSynced);

    Manager legacy;
    legacy.Restore({{7, Kind::kAlarm, Repeat::kOnce, "旧闹铃", when, {}, "", ""}}, 8);
    const auto* migrated = legacy.Find(7);
    assert(migrated != nullptr);
    assert(migrated->source_schedule_id == "7");
    assert(migrated->schedule_uuid.empty());
    assert(migrated->schedule_version == 0);
    assert(migrated->sync_state == SyncState::kPending);
}

void TestOfflineOccurrenceOutboxAndAuthorityAck() {
    const auto when = At(2026, 8, 14, 9);
    Manager manager;
    manager.Tick(when - 10, true);
    auto task = Add(manager, Kind::kAlarm, Repeat::kOnce, when);
    auto due = manager.Tick(when, true);
    assert(due.triggered.size() == 1);
    assert(manager.pending_occurrences().size() == 1);
    const auto& pending = manager.pending_occurrences().front();
    assert(pending.source_schedule_id == std::to_string(task.id));
    assert(pending.occurrence_at == when);

    // Retrying the same local occurrence must not create another durable record.
    manager.RecordOccurrence(due.triggered.front(), when);
    assert(manager.pending_occurrences().size() == 1);
    assert(!manager.AcknowledgeOccurrence("", task.id + 100, when));
    assert(manager.AcknowledgeOccurrence("", task.id, when));
    assert(manager.pending_occurrences().empty());
}

void TestCompleteAuthoritySnapshotReconciliation() {
    const auto first_time = At(2026, 8, 14, 10);
    Manager manager;
    auto pending = Add(manager, Kind::kReminder, Repeat::kOnce, first_time);

    AuthorityTask own;
    own.schedule_uuid = "4d8b9aec-1e54-4d3e-a9f4-c37a7f21c995";
    own.source_schedule_id = std::to_string(pending.id);
    own.kind = Kind::kReminder;
    own.repeat = Repeat::kOnce;
    own.label = "服务端修改后的提醒";
    own.trigger_at = first_time + 60;
    own.version = 2;
    own.local_source = true;
    own.revision = 6;

    AuthorityTask imported;
    imported.schedule_uuid = "b79cecd0-1c67-47db-a676-fffc37e24f17";
    imported.source_schedule_id = "88";
    imported.kind = Kind::kAlarm;
    imported.repeat = Repeat::kDaily;
    imported.label = "共享闹铃";
    imported.trigger_at = first_time + 120;
    imported.version = 1;
    imported.local_source = false;
    imported.revision = 7;

    auto first = manager.ApplyAuthorityChanges(7, {own, imported});
    assert(first.changed);
    assert(manager.snapshot_revision() == 7);
    assert(manager.tasks().size() == 2);
    const auto* rebound = manager.Find(pending.id);
    assert(rebound != nullptr && rebound->label == "服务端修改后的提醒");
    assert(rebound->schedule_uuid == own.schedule_uuid);
    assert(rebound->schedule_version == 2);
    assert(rebound->sync_state == SyncState::kSynced);

    // A stale snapshot is ignored, and a newer complete snapshot removes only
    // authoritative copies absent from it. Unsynced local work remains offline-safe.
    assert(!manager.ApplyAuthorityChanges(7, {}).changed);
    auto local_only = Add(manager, Kind::kReminder, Repeat::kOnce, first_time + 180);
    AuthorityTask removed = imported;
    removed.deleted = true;
    removed.revision = 8;
    removed.version = 2;
    auto second = manager.ApplyAuthorityChanges(8, {removed});
    assert(second.changed);
    assert(manager.Find(local_only.id) != nullptr);
    assert(manager.tasks().size() == 2);
    assert(manager.FindByScheduleUuid(imported.schedule_uuid) == nullptr);

    AuthorityTask recurring_snooze;
    recurring_snooze.revision = 9;
    recurring_snooze.schedule_uuid = "d2719f87-5bc2-40c9-8978-c3904334019a";
    recurring_snooze.source_schedule_id = "99";
    recurring_snooze.kind = Kind::kReminder;
    recurring_snooze.repeat = Repeat::kDaily;
    recurring_snooze.label = "重复提醒";
    recurring_snooze.trigger_at = first_time + 86400;
    recurring_snooze.snoozed_until = first_time + 300;
    recurring_snooze.version = 2;
    assert(manager.ApplyAuthorityChanges(9, {recurring_snooze}).changed);
    assert(std::count_if(manager.tasks().begin(), manager.tasks().end(),
        [&](const Task& task) {
            return task.schedule_uuid == recurring_snooze.schedule_uuid;
        }) == 2);
}

void TestAuthorityActionIsIdempotentAndSnoozesTriggeredTask() {
    const auto when = At(2026, 8, 14, 11);
    Manager manager;
    auto active = Add(manager, Kind::kAlarm, Repeat::kOnce, when);
    assert(manager.BindAuthority(active.id,
        "4d8b9aec-1e54-4d3e-a9f4-c37a7f21c995", 1));
    const std::string schedule_uuid = manager.Find(active.id)->schedule_uuid;
    manager.Tick(when - 1, true);
    auto due = manager.Tick(when, true);
    assert(due.triggered.size() == 1 && manager.tasks().empty());
    assert(manager.ApplyAuthorityAction(9, schedule_uuid, 2, "snooze",
                                        when + 300, 0, &due.triggered.front()));
    const auto* snoozed = manager.FindByScheduleUuid(schedule_uuid);
    assert(snoozed != nullptr && snoozed->trigger_at == when + 300);
    assert(snoozed->repeat == Repeat::kOnce && snoozed->enabled);
    assert(!manager.ApplyAuthorityAction(9, schedule_uuid, 2, "snooze",
                                         when + 300, 0, nullptr));
    assert(manager.ApplyAuthorityAction(10, schedule_uuid, 3, "stop", 0, 0));
    assert(manager.FindByScheduleUuid(schedule_uuid) == nullptr);
    assert(manager.List(KindFilter::kAlarm).empty());
    assert(manager.List(KindFilter::kAlarm, false).empty());

    Manager recurring;
    auto daily = Add(recurring, Kind::kAlarm, Repeat::kDaily, when);
    assert(recurring.BindAuthority(daily.id,
        "b79cecd0-1c67-47db-a676-fffc37e24f17", 1));
    const auto tomorrow = when + 86400;
    assert(recurring.ApplyAuthorityAction(
        1, "b79cecd0-1c67-47db-a676-fffc37e24f17", 2, "stop", 0, tomorrow));
    const auto* next = recurring.FindByScheduleUuid(
        "b79cecd0-1c67-47db-a676-fffc37e24f17");
    assert(next != nullptr && next->enabled && next->trigger_at == tomorrow);

    Manager recurring_snooze;
    auto recurring_task = Add(recurring_snooze, Kind::kReminder, Repeat::kDaily, when);
    const std::string recurring_uuid = "d2719f87-5bc2-40c9-8978-c3904334019a";
    assert(recurring_snooze.BindAuthority(recurring_task.id, recurring_uuid, 1));
    assert(recurring_snooze.ApplyAuthorityAction(
        1, recurring_uuid, 2, "snooze", when + 300, tomorrow));
    assert(recurring_snooze.tasks().size() == 2);
    assert(std::any_of(recurring_snooze.tasks().begin(), recurring_snooze.tasks().end(),
        [&](const Task& task) {
            return task.schedule_uuid == recurring_uuid && task.repeat == Repeat::kDaily &&
                task.trigger_at == tomorrow;
        }));
    assert(std::any_of(recurring_snooze.tasks().begin(), recurring_snooze.tasks().end(),
        [&](const Task& task) {
            return task.schedule_uuid == recurring_uuid && task.repeat == Repeat::kOnce &&
                task.trigger_at == when + 300;
        }));
}

void TestSnoozeKeepsAuthorityIdentityDirty() {
    const auto when = At(2026, 8, 14, 12);
    Manager manager;
    auto task = Add(manager, Kind::kReminder, Repeat::kOnce, when);
    assert(manager.BindAuthority(task.id,
        "4d8b9aec-1e54-4d3e-a9f4-c37a7f21c995", 4));
    const auto source = *manager.Find(task.id);
    const auto snoozed = manager.Snooze(source, 5, when);
    assert(snoozed.source_schedule_id == source.source_schedule_id);
    assert(snoozed.schedule_uuid == source.schedule_uuid);
    assert(snoozed.schedule_version == source.schedule_version);
    assert(snoozed.sync_state == SyncState::kDirty);
}

void TestRemoteSourceIdCannotBindOrRemoveLocalTask() {
    const auto when = At(2026, 8, 14, 13);
    Manager manager;
    auto local = Add(manager, Kind::kReminder, Repeat::kOnce, when);
    AuthorityTask remote;
    remote.revision = 1;
    remote.schedule_uuid = "4d8b9aec-1e54-4d3e-a9f4-c37a7f21c995";
    remote.source_schedule_id = local.source_schedule_id;
    remote.kind = Kind::kAlarm;
    remote.repeat = Repeat::kOnce;
    remote.label = "其他音箱任务";
    remote.trigger_at = when + 60;
    remote.version = 1;
    remote.local_source = false;
    assert(manager.ApplyAuthorityChanges(1, {remote}).changed);
    assert(manager.tasks().size() == 2);
    assert(manager.Find(local.id)->schedule_uuid.empty());

    AlertQueue queue;
    queue.Enqueue(*manager.Find(local.id));
    assert(queue.Remove(remote.schedule_uuid, remote.source_schedule_id, false) == 0);
    assert(queue.StartNext()->id == local.id);
}

void TestOccurrenceOutboxBackpressureAndTerminalCleanup() {
    const auto when = At(2026, 8, 14, 14);
    Task due{1, Kind::kAlarm, Repeat::kOnce, "不能丢的闹铃", when, {}, "", ""};
    due.source_schedule_id = "1";
    std::vector<PendingOccurrence> full;
    for (uint32_t id = 10; id < 10 + Manager::kMaxPendingOccurrences; ++id) {
        full.push_back({id, std::to_string(id), "", Kind::kReminder, "离线提醒", "", "",
                        when - static_cast<std::time_t>(id)});
    }
    Manager manager;
    manager.Restore({due}, 2, full);
    auto blocked = manager.Tick(when, true);
    assert(!blocked.occurrence_outbox_full);
    assert(blocked.triggered.size() == 1);
    assert(manager.Find(1) == nullptr);
    assert(manager.pending_occurrences().size() == Manager::kMaxPendingOccurrences);
    assert(manager.overflow_occurrences().size() == 1);

    assert(manager.AcknowledgeOccurrence("", 10, when));
    assert(manager.overflow_occurrences().empty());

    Manager cleanup;
    auto task = Add(cleanup, Kind::kReminder, Repeat::kOnce, when + 100);
    assert(cleanup.BindAuthority(task.id,
        "b79cecd0-1c67-47db-a676-fffc37e24f17", 1));
    cleanup.Tick(when, true);
    auto triggered = cleanup.Tick(when + 100, true);
    assert(triggered.triggered.size() == 1 && cleanup.pending_occurrences().size() == 1);
    assert(cleanup.ApplyAuthorityAction(
        1, "b79cecd0-1c67-47db-a676-fffc37e24f17", 2, "complete", 0, 0));
    assert(cleanup.pending_occurrences().empty());
}

void TestPagedFullSnapshotOnlyReconcilesOnFinalPage() {
    const auto when = At(2026, 8, 14, 15);
    Manager manager;
    auto absent = Add(manager, Kind::kAlarm, Repeat::kOnce, when);
    const std::string absent_uuid = "4d8b9aec-1e54-4d3e-a9f4-c37a7f21c995";
    assert(manager.BindAuthority(absent.id, absent_uuid, 1));
    auto local_pending = Add(manager, Kind::kReminder, Repeat::kOnce, when + 60);

    AuthorityTask first;
    first.revision = 1;
    first.schedule_uuid = "b79cecd0-1c67-47db-a676-fffc37e24f17";
    first.source_schedule_id = "81";
    first.kind = Kind::kReminder;
    first.repeat = Repeat::kOnce;
    first.label = "第一页";
    first.trigger_at = when + 120;
    first.version = 1;
    auto page_one = manager.ApplyAuthorityChanges(1, {first}, true, true);
    assert(page_one.changed && page_one.removed_local_ids.empty());
    assert(manager.full_snapshot_in_progress());
    assert(manager.FindByScheduleUuid(absent_uuid) != nullptr);

    Manager restored;
    restored.Restore(manager.tasks(), manager.next_id(), manager.pending_occurrences(),
                     manager.snapshot_revision(), manager.action_revision(),
                     manager.full_snapshot_in_progress(),
                     manager.full_snapshot_seen_uuids(),
                     manager.full_snapshot_staged_tasks());
    assert(restored.full_snapshot_in_progress());
    assert(restored.full_snapshot_seen_uuids().size() == 1);

    AuthorityTask second = first;
    second.revision = 2;
    second.schedule_uuid = "d2719f87-5bc2-40c9-8978-c3904334019a";
    second.source_schedule_id = "82";
    second.label = "末页";
    auto page_two = restored.ApplyAuthorityChanges(2, {second}, false, false);
    assert(page_two.changed);
    assert(!restored.full_snapshot_in_progress());
    assert(restored.full_snapshot_seen_uuids().empty());
    assert(restored.FindByScheduleUuid(absent_uuid) == nullptr);
    assert(restored.Find(local_pending.id) != nullptr);
    assert(restored.FindByScheduleUuid(first.schedule_uuid) != nullptr);
    assert(restored.FindByScheduleUuid(second.schedule_uuid) != nullptr);
}

void TestEmptyFullSnapshotDropsOldAccountButKeepsLocalPending() {
    const auto when = At(2026, 8, 14, 16);
    Manager manager;
    auto old_account = Add(manager, Kind::kAlarm, Repeat::kOnce, when);
    assert(manager.BindAuthority(old_account.id,
        "4d8b9aec-1e54-4d3e-a9f4-c37a7f21c995", 1));
    auto local_pending = Add(manager, Kind::kReminder, Repeat::kOnce, when + 60);

    auto result = manager.ApplyAuthorityChanges(0, {}, true, false);
    assert(result.changed && result.removed_local_ids.size() == 1);
    assert(manager.Find(old_account.id) == nullptr);
    assert(manager.Find(local_pending.id) != nullptr);
    assert(manager.snapshot_revision() == 0);
}

void TestFailedFullSnapshotPageDoesNotPartiallyClean() {
    const auto when = At(2026, 8, 14, 17);
    Manager manager;
    auto retained = Add(manager, Kind::kAlarm, Repeat::kOnce, when);
    const std::string retained_uuid = "4d8b9aec-1e54-4d3e-a9f4-c37a7f21c995";
    assert(manager.BindAuthority(retained.id, retained_uuid, 1));
    for (size_t i = manager.tasks().size(); i < Manager::kMaxTasks; ++i) {
        Add(manager, Kind::kReminder, Repeat::kOnce,
            when + static_cast<std::time_t>(i + 1) * 60);
    }

    AuthorityTask seen;
    seen.revision = 1;
    seen.schedule_uuid = retained_uuid;
    seen.source_schedule_id = retained.source_schedule_id;
    seen.kind = Kind::kAlarm;
    seen.repeat = Repeat::kOnce;
    seen.label = retained.label;
    seen.trigger_at = retained.trigger_at;
    seen.version = 1;
    seen.local_source = true;
    assert(manager.ApplyAuthorityChanges(1, {seen}, true, true).changed);
    const auto before_tasks = manager.tasks();
    const auto before_seen = manager.full_snapshot_seen_uuids();

    AuthorityTask overflow = seen;
    overflow.revision = 2;
    overflow.schedule_uuid = "b79cecd0-1c67-47db-a676-fffc37e24f17";
    overflow.source_schedule_id = "99";
    overflow.local_source = false;
    bool rejected = false;
    try {
        manager.ApplyAuthorityChanges(2, {overflow}, false, false);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    assert(rejected);
    assert(manager.tasks().size() == before_tasks.size());
    assert(manager.FindByScheduleUuid(retained_uuid) != nullptr);
    assert(manager.full_snapshot_in_progress());
    assert(manager.snapshot_revision() == 1);
    assert(manager.full_snapshot_seen_uuids() == before_seen);
}

void TestFullSnapshotCanReplaceFullOldAccount() {
    const auto when = At(2026, 8, 14, 18);
    Manager manager;
    for (size_t i = 0; i < Manager::kMaxTasks; ++i) {
        auto old = Add(manager, Kind::kReminder, Repeat::kOnce,
                       when + static_cast<std::time_t>(i) * 60);
        char uuid[37];
        snprintf(uuid, sizeof(uuid), "00000000-0000-4000-8000-%012zx", i + 1);
        assert(manager.BindAuthority(old.id, uuid, 1));
    }
    AuthorityTask replacement;
    replacement.revision = 1;
    replacement.schedule_uuid = "4d8b9aec-1e54-4d3e-a9f4-c37a7f21c995";
    replacement.source_schedule_id = "901";
    replacement.kind = Kind::kAlarm;
    replacement.repeat = Repeat::kOnce;
    replacement.label = "新账号闹铃";
    replacement.trigger_at = when + 3600;
    replacement.version = 1;
    auto result = manager.ApplyAuthorityChanges(1, {replacement}, true, false);
    assert(result.changed && result.removed_local_ids.size() == Manager::kMaxTasks);
    assert(manager.tasks().size() == 1);
    assert(manager.FindByScheduleUuid(replacement.schedule_uuid) != nullptr);
}

void TestOccurrenceOverflowStillRingsAndSurvivesRestart() {
    const auto when = At(2026, 8, 14, 19);
    Task due{1, Kind::kBriefing, Repeat::kOnce, "溢出简报仍响", when, {},
             "weather,news", "广州"};
    due.source_schedule_id = "1";
    std::vector<PendingOccurrence> full;
    for (uint32_t id = 100; id < 100 + Manager::kMaxPendingOccurrences; ++id) {
        full.push_back({id, std::to_string(id), "", Kind::kReminder, "待补报", "", "",
                        when - static_cast<std::time_t>(id)});
    }
    Manager manager;
    manager.Restore({due}, 2, full);
    auto triggered = manager.Tick(when, true);
    assert(triggered.triggered.size() == 1);
    assert(!triggered.occurrence_outbox_full);
    assert(manager.tasks().empty());
    assert(manager.overflow_occurrences().size() == 1);

    Manager restored;
    restored.Restore(manager.tasks(), manager.next_id(), manager.pending_occurrences(),
                     manager.snapshot_revision(), manager.action_revision(), false, {}, {},
                     manager.overflow_occurrences());
    assert(restored.Tick(when + 1, true).triggered.empty());
    assert(restored.AcknowledgeOccurrence("", 100, when));
    assert(restored.overflow_occurrences().empty());
    assert(restored.pending_occurrences().size() == Manager::kMaxPendingOccurrences);
    const auto promoted = std::find_if(restored.pending_occurrences().begin(),
        restored.pending_occurrences().end(), [](const auto& occurrence) {
            return occurrence.local_id == 1;
        });
    assert(promoted != restored.pending_occurrences().end());
    assert(promoted->kind == Kind::kBriefing);
    assert(promoted->sections == "weather,news" && promoted->location == "广州");
}

int main() {
    setenv("TZ", "UTC", 1);
    tzset();
    TestLimitDeleteAndClear();
    TestFiveRepeatsAndCrossDay();
    TestKindFiltersAndScopedClear();
    TestFirstRepeatConstraintAndFirstTick();
    TestNaturalTaskDescription();
    TestShortCreationDescription();
    TestTemporaryVolumeRestoreDecision();
    TestWhitespaceLabels();
    TestRecoveryAndOrderingAndDeduplication();
    TestInvalidTimeStopAndSnooze();
    TestReminderDeliverySequence();
    TestBriefingValidationAndFilter();
    TestRepeatingSuggestionUsesRealTasks();
    TestSharedScheduleMetadataAndLegacyMigration();
    TestOfflineOccurrenceOutboxAndAuthorityAck();
    TestCompleteAuthoritySnapshotReconciliation();
    TestAuthorityActionIsIdempotentAndSnoozesTriggeredTask();
    TestSnoozeKeepsAuthorityIdentityDirty();
    TestRemoteSourceIdCannotBindOrRemoveLocalTask();
    TestOccurrenceOutboxBackpressureAndTerminalCleanup();
    TestPagedFullSnapshotOnlyReconcilesOnFinalPage();
    TestEmptyFullSnapshotDropsOldAccountButKeepsLocalPending();
    TestFailedFullSnapshotPageDoesNotPartiallyClean();
    TestFullSnapshotCanReplaceFullOldAccount();
    TestOccurrenceOverflowStillRingsAndSurvivesRestart();
    return 0;
}
