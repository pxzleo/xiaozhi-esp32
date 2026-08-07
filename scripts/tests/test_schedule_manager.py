import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


class ScheduleManagerTest(unittest.TestCase):
    def test_host_schedule_core(self):
        if not shutil.which("g++"):
            self.skipTest("a host C++ compiler is unavailable")
        source = ROOT / "main" / "schedule" / "schedule_manager.cc"
        test = ROOT / "scripts" / "tests" / "cpp" / "schedule_manager_test.cc"
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "schedule_manager_test"
            subprocess.run(
                [
                    "g++",
                    "-std=c++17",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    f"-I{source.parent}",
                    str(source),
                    str(test),
                    "-o",
                    str(executable),
                ],
                check=True,
            )
            subprocess.run([str(executable)], check=True)

    def test_device_integration_contract(self):
        application = (ROOT / "main" / "application.cc").read_text(encoding="utf-8")
        mcp = (ROOT / "main" / "mcp_server.cc").read_text(encoding="utf-8")
        board = (
            ROOT
            / "main/boards/waveshare/esp32-s3-touch-amoled-1.32"
            / "esp32-s3-touch-amoled-1.32.cc"
        ).read_text(encoding="utf-8")
        self.assertIn('"notifications/schedule/triggered"', application)
        self.assertIn("speak", application)
        for tool in ("create", "list", "delete", "clear", "stop", "snooze"):
            self.assertIn(f'"self.schedule.{tool}"', mcp)
        self.assertGreaterEqual(
            mcp.count('Property("kind", kPropertyTypeString, std::string("all"))'), 2
        )
        self.assertIn("Manager::DescribeTask", application)
        self.assertIn("ParseKindFilter", application)
        self.assertIn("std::unique_ptr<cJSON, decltype(&cJSON_Delete)>", application)
        self.assertIn("last_load_error", application)
        self.assertIn("schedule_manager_.Restore({}, 1)", application)
        self.assertIn("now_us >= schedule_alert_deadline_us_", application)
        self.assertIn("kScheduleAlertMinimumVolume = 80", application)
        self.assertIn("kReminderCueRepeats = 2", application)
        self.assertIn("kReminderTtsStartTimeoutSeconds = 15", application)
        self.assertIn("reminder_delivery_.OnPlaybackDrained()", application)
        self.assertIn("reminder_delivery_.OnTtsStarted()", application)
        self.assertIn("reminder_delivery_.OnTtsStopped()", application)
        self.assertIn("reminder_delivery_.CancelWaitingForTts()", application)
        drained_handler = application.split(
            "if (bits & MAIN_EVENT_PLAYBACK_DRAINED)", 1
        )[1].split("if (bits & MAIN_EVENT_TOGGLE_CHAT)", 1)[0]
        self.assertLess(
            drained_handler.index("reminder_delivery_.OnPlaybackDrained()"),
            drained_handler.index("NotifyReminderTriggered(active_schedule_task_"),
        )
        start_alert = application.split("void Application::StartNextScheduleAlert()", 1)[1]
        start_alert = start_alert.split("void Application::RestoreScheduleAlertVolume()", 1)[0]
        self.assertNotIn("NotifyReminderTriggered", start_alert)
        self.assertIn("FinishScheduleAlert(active_schedule_task_.kind == schedule::Kind::kAlarm,", application)
        self.assertIn("!reminder_delivery_pending", application)
        self.assertIn(
            "reminder_delivery_.state() == schedule::ReminderDeliveryState::kInactive",
            application,
        )
        stop_alert = application.split("bool Application::TryStopScheduleAlert()", 1)[1]
        stop_alert = stop_alert.split("std::string Application::SnoozeScheduleAlert", 1)[0]
        self.assertLess(
            stop_alert.index("AbortSpeaking(kAbortReasonNone)"),
            stop_alert.index("FinishScheduleAlert()"),
        )
        snooze_alert = application.split("std::string Application::SnoozeScheduleAlert", 1)[1]
        snooze_alert = snooze_alert.split("void Application::ResetProtocol", 1)[0]
        self.assertLess(
            snooze_alert.index("AbortSpeaking(kAbortReasonNone)"),
            snooze_alert.index("FinishScheduleAlert()"),
        )
        self.assertIn("netease_lyrics_.Clear()", application)
        self.assertIn("CloseNeteaseMusicLyrics()", application)
        self.assertIn("TryStopScheduleAlert", board)
        self.assertNotIn("IsScheduleAlertActive", board)

        audio = (ROOT / "main" / "audio" / "audio_codec.cc").read_text(encoding="utf-8")
        transient = audio.split("void AudioCodec::SetOutputVolumeTransient", 1)[1]
        transient = transient.split("void AudioCodec::SetInputGain", 1)[0]
        self.assertNotIn("Settings", transient)
        self.assertIn("suppress_volume_persistence_", transient)


if __name__ == "__main__":
    unittest.main()
