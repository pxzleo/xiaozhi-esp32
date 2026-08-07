#include "schedule_manager.h"

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

void TestRecoveryAndOrderingAndDeduplication() {
    const auto now = At(2026, 8, 7, 10);
    Manager recovery;
    recovery.Restore({{1, Kind::kAlarm, Repeat::kOnce, "补响", now - 300, {}},
                      {2, Kind::kReminder, Repeat::kOnce, "错过", now - 301, {}},
                      {3, Kind::kAlarm, Repeat::kDaily, "重复", At(2026, 1, 1, 9), {}}},
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
    auto due = manager.Tick(now, true);
    assert(due.triggered.size() == 3);
    assert(due.triggered[0].id == alarm2.id);
    assert(due.triggered[1].id == alarm1.id);
    assert(due.triggered[2].id == reminder.id);
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

int main() {
    setenv("TZ", "UTC", 1);
    tzset();
    TestLimitDeleteAndClear();
    TestFiveRepeatsAndCrossDay();
    TestKindFiltersAndScopedClear();
    TestFirstRepeatConstraintAndFirstTick();
    TestNaturalTaskDescription();
    TestRecoveryAndOrderingAndDeduplication();
    TestInvalidTimeStopAndSnooze();
    return 0;
}
