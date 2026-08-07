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
        boards = [
            (
                ROOT
                / "main/boards/waveshare/esp32-s3-touch-amoled-1.32"
                / "esp32-s3-touch-amoled-1.32.cc"
            ).read_text(encoding="utf-8"),
            (
                ROOT
                / "main/boards/waveshare/esp32-s3-touch-lcd-1.85c"
                / "esp32-s3-touch-lcd-1.85c.cc"
            ).read_text(encoding="utf-8"),
        ]
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
        tts_start = application.split(
            'if (strcmp(state->valuestring, "start") == 0)', 1
        )[1].split('strcmp(state->valuestring, "stop")', 1)[0]
        self.assertLess(
            tts_start.index("ReminderDeliveryState::kWaitingForCue"),
            tts_start.index("SetDeviceState(kDeviceStateSpeaking)"),
        )
        self.assertIn("return;", tts_start)
        drained_handler = application.split(
            "if (bits & MAIN_EVENT_PLAYBACK_DRAINED)", 1
        )[1].split("if (bits & MAIN_EVENT_TOGGLE_CHAT)", 1)[0]
        self.assertNotIn(
            "active_schedule_task_.kind == schedule::Kind::kReminder",
            drained_handler,
        )
        self.assertLess(
            drained_handler.index("reminder_delivery_.OnPlaybackDrained()"),
            drained_handler.index("NotifyReminderTriggered(active_schedule_task_"),
        )
        start_alert = application.split("void Application::StartNextScheduleAlert()", 1)[1]
        start_alert = start_alert.split("void Application::RestoreScheduleAlertVolume()", 1)[0]
        self.assertNotIn("NotifyReminderTriggered", start_alert)
        self.assertLess(
            start_alert.index("reminder_delivery_.Begin()"),
            start_alert.index("SetDeviceState(kDeviceStateIdle)"),
        )
        self.assertLess(
            start_alert.index("SetDeviceState(kDeviceStateIdle)"),
            start_alert.index("audio_service_.PlaySound"),
        )
        self.assertLess(
            start_alert.index("EnableVoiceProcessing(false)"),
            start_alert.index("audio_service_.PlaySound"),
        )
        self.assertLess(
            start_alert.index("EnableWakeWordDetection(false)"),
            start_alert.index("audio_service_.PlaySound"),
        )
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
        self.assertLess(
            stop_alert.index("FinishScheduleAlert()"),
            stop_alert.index("SetDeviceState(kDeviceStateIdle)"),
        )
        snooze_alert = application.split("std::string Application::SnoozeScheduleAlert", 1)[1]
        snooze_alert = snooze_alert.split("void Application::ResetProtocol", 1)[0]
        self.assertLess(
            snooze_alert.index("AbortSpeaking(kAbortReasonNone)"),
            snooze_alert.index("FinishScheduleAlert()"),
        )
        self.assertLess(
            snooze_alert.index("FinishScheduleAlert()"),
            snooze_alert.index("SetDeviceState(kDeviceStateIdle)"),
        )
        self.assertIn("netease_lyrics_.Clear()", application)
        self.assertIn("CloseNeteaseMusicLyrics()", application)
        for board in boards:
            self.assertIn("TryStopScheduleAlert", board)
            self.assertNotIn("IsScheduleAlertActive", board)
        self.assertIn("开始收听键停止", application)
        idle_state = application.split("case kDeviceStateIdle:", 1)[1]
        idle_state = idle_state.split("case kDeviceStateConnecting:", 1)[0]
        self.assertIn("EnableWakeWordDetection(!schedule_alert_active_)", idle_state)
        for board in boards:
            boot_click = board.split("boot_button_.OnClick", 1)[1]
            self.assertLess(
                boot_click.index("TryStopScheduleAlert"),
                boot_click.index("ToggleChatState"),
            )

        alarm_repeat = application.split(
            "active_schedule_task_.kind == schedule::Kind::kAlarm &&", 1
        )[1].split("void Application::StartNextScheduleAlert", 1)[0]
        self.assertIn("ReminderDeliveryState::kInactive", alarm_repeat)

        audio = (ROOT / "main" / "audio" / "audio_codec.cc").read_text(encoding="utf-8")
        transient = audio.split("void AudioCodec::SetOutputVolumeTransient", 1)[1]
        transient = transient.split("void AudioCodec::SetInputGain", 1)[0]
        self.assertNotIn("Settings", transient)
        self.assertIn("suppress_volume_persistence_", transient)

    def test_reminder_reopens_channel_before_notification(self):
        application = (ROOT / "main" / "application.cc").read_text(encoding="utf-8")
        protocol_h = (ROOT / "main" / "protocols" / "protocol.h").read_text(
            encoding="utf-8"
        )
        protocol_cc = (ROOT / "main" / "protocols" / "protocol.cc").read_text(
            encoding="utf-8"
        )
        websocket = (
            ROOT / "main" / "protocols" / "websocket_protocol.cc"
        ).read_text(encoding="utf-8")
        notify = application.split("Application::NotifyReminderTriggered", 1)[1]
        notify = notify.split("void Application::CheckSchedules", 1)[0]
        self.assertIn("protocol_->IsAudioChannelOpened()", notify)
        self.assertIn("protocol_->OpenAudioChannel()", notify)
        self.assertLess(
            notify.index("protocol_->OpenAudioChannel()"),
            notify.index("protocol_->SendMcpMessage("),
        )
        self.assertIn("bool SendMcpMessage", protocol_h)
        self.assertIn("return SendText(message);", protocol_cc)
        self.assertIn("return protocol_->SendMcpMessage(", notify)

        error_handler = application.split("if (bits & MAIN_EVENT_ERROR)", 1)[1]
        error_handler = error_handler.split("if (bits & MAIN_EVENT_NETWORK_CONNECTED)", 1)[0]
        self.assertIn("schedule_alert_active_", error_handler)
        self.assertIn("Reminder network error", error_handler)

        closed_handler = application.split("protocol_->OnAudioChannelClosed", 1)[1]
        closed_handler = closed_handler.split("protocol_->OnIncomingJson", 1)[0]
        self.assertIn("reminder_delivery_.Reset()", closed_handler)
        self.assertIn("if (!schedule_alert_active_)", closed_handler)

        open_channel = websocket.split("bool WebsocketProtocol::OpenAudioChannel()", 1)[1]
        open_channel = open_channel.split("std::string WebsocketProtocol::GetHelloMessage", 1)[0]
        self.assertIn("session_id_.clear()", open_channel)
        self.assertIn("xEventGroupClearBits", open_channel)
        self.assertIn("connection_generation_", open_channel)


if __name__ == "__main__":
    unittest.main()
