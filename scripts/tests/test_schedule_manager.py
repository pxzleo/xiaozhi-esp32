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

    def test_host_proactive_core(self):
        if not shutil.which("g++"):
            self.skipTest("a host C++ compiler is unavailable")
        source = ROOT / "main" / "proactive" / "proactive_manager.cc"
        test = ROOT / "scripts" / "tests" / "cpp" / "proactive_manager_test.cc"
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "proactive_manager_test"
            subprocess.run(
                [
                    "g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    f"-I{source.parent}", str(source), str(test), "-o", str(executable),
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
        self.assertIn('"notifications/assistant/triggered"', application)
        self.assertIn("kBriefingTtsStartTimeoutSeconds = 30", application)
        self.assertIn('Property("sections", kPropertyTypeString', mcp)
        self.assertIn("speak", application)
        for tool in ("create", "list", "delete", "clear", "stop", "snooze"):
            self.assertIn(f'"self.schedule.{tool}"', mcp)
        for tool in ("complete_recent", "follow_up", "dismiss_follow_up"):
            self.assertIn(f'"self.schedule.{tool}"', mcp)
        for tool in ("configure", "status", "mute", "allow_topic", "block_topic"):
            self.assertIn(f'"self.proactive.{tool}"', mcp)
        self.assertIn("不要先说‘我来处理一下’", mcp)
        self.assertIn('"notifications/schedule/follow_up"', application)
        self.assertIn('"notifications/device/health"', application)
        self.assertIn('"follow_up", true', application)
        self.assertIn('"source_id"', application)
        proactive_check = application.split("void Application::CheckProactiveEvents()", 1)[1]
        proactive_check = proactive_check.split("bool Application::NotifyReminderTriggered", 1)[0]
        self.assertLess(
            proactive_check.index("pending_proactive_event_ = std::move(*event)"),
            proactive_check.index("audio_service_.PlaySound(Lang::Sounds::OGG_POPUP)"),
        )
        drained = application.split(
            "if (pending_proactive_event_ && audio_service_.IsPlaybackIdle())", 1
        )[1].split("if (bits & MAIN_EVENT_TOGGLE_CHAT)", 1)[0]
        self.assertIn("SendProactiveEvent(event)", drained)
        send_proactive = application.split("bool Application::SendProactiveEvent", 1)[1]
        send_proactive = send_proactive.split("std::string Application::FindFollowUpLabel", 1)[0]
        self.assertIn("protocol_->IsAudioChannelOpened()", send_proactive)
        self.assertNotIn("OpenAudioChannel", send_proactive)
        self.assertIn("PendingDue(now)", proactive_check)
        self.assertLess(
            proactive_check.index("proactive_queue_.Push(event)"),
            proactive_check.index("schedule_follow_ups_.MarkAsked"),
        )
        self.assertIn("LoadProactive()", application)
        self.assertIn("SaveProactive()", application)
        self.assertIn("time_unsynchronized", application)
        self.assertIn("uptime_ticks_ >= 600", application)
        self.assertIn("RemoveByDedupeKey", application)
        self.assertIn("active_schedule_task_.trigger_at, now", application)
        self.assertIn("proactive_retry_backoff_.Ready", application)
        self.assertIn("RecordProactiveSendResult", application)
        self.assertIn("kMaxSerializedBytes = 3600", application)
        self.assertIn("nvs_get_stats(nullptr, &nvs_stats)", application)
        self.assertIn("kNvsSafetyEntries = 16", application)
        self.assertIn("TrySaveProactive", application)
        self.assertIn("proactive_save_pending_ = true", application)
        self.assertIn("未应用修改", application)
        self.assertEqual(application.count("SaveProactive();"), 1)
        self.assertIn("recovered_health ||", application)
        health_queue = application.split("void Application::QueueHealthEvent", 1)[1]
        health_queue = health_queue.split("bool Application::SendProactiveEvent", 1)[0]
        self.assertIn("audio_service_.PlaySound(Lang::Sounds::OGG_POPUP)", health_queue)
        network_callback = application.split("case NetworkEvent::Scanning:", 1)[1]
        scanning = network_callback.split("case NetworkEvent::Connecting", 1)[0]
        self.assertNotIn("MAIN_EVENT_NETWORK_DISCONNECTED", scanning)
        disconnected = application.split("case NetworkEvent::Disconnected:", 1)[1]
        disconnected = disconnected.split("case NetworkEvent::WifiConfigModeEnter", 1)[0]
        self.assertIn("network_connected_.exchange(false)", disconnected)
        self.assertIn("network_disconnect_us_.push_back(esp_timer_get_time())", disconnected)
        self.assertIn("network_flapping", application)
        self.assertIn("ota_update_available", application)
        audio_h = (ROOT / "main" / "audio" / "audio_service.h").read_text(encoding="utf-8")
        audio_cc = (ROOT / "main" / "audio" / "audio_service.cc").read_text(encoding="utf-8")
        self.assertIn("on_critical_error", audio_h)
        self.assertIn("audio_decode_failed", audio_cc)
        set_callbacks = audio_cc.split("void AudioService::SetCallbacks", 1)[1]
        set_callbacks = set_callbacks.split("void AudioService::PlaySound", 1)[0]
        self.assertIn("decode_health_failed_.load()", set_callbacks)
        self.assertIn("callbacks_.on_critical_error", set_callbacks)
        self.assertGreaterEqual(
            mcp.count('Property("kind", kPropertyTypeString, std::string("all"))'), 2
        )
        self.assertIn("Manager::DescribeTask", application)
        self.assertIn("Manager::DescribeCreation(task, now)", application)
        self.assertIn("convert_to_repeating_schedule", application)
        self.assertIn("用户只说‘晚点、有空、回头’", mcp)
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
            tts_start.index("!reminder_delivery_.OnTtsStarted()"),
            tts_start.index("SetDeviceState(kDeviceStateSpeaking)"),
        )
        self.assertIn("Ignoring unexpected TTS start for schedule alert", tts_start)
        tts_stop = application.split(
            'strcmp(state->valuestring, "stop") == 0', 1
        )[1].split('strcmp(state->valuestring, "sentence_start")', 1)[0]
        self.assertLess(
            tts_stop.index("reminder_delivery_.OnTtsStopped()"),
            tts_stop.index("FinishScheduleAlert()"),
        )
        self.assertLess(
            tts_stop.index("FinishScheduleAlert()"),
            tts_stop.index("SetDeviceState(kDeviceStateListening)"),
        )
        timeout_handler = application.split(
            "now_us >= schedule_reminder_tts_deadline_us_", 1
        )[1].split("if (!schedule_alert_active_", 1)[0]
        self.assertLess(
            timeout_handler.index("CancelWaitingForTts()"),
            timeout_handler.index("AbortSpeaking(kAbortReasonNone)"),
        )
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
        self.assertIn('"已延后" + std::to_string(minutes) + "分钟。"', snooze_alert)
        self.assertNotIn("DescribeTask", snooze_alert)
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
        self.assertIn(
            "reminder_delivery_.state() != schedule::ReminderDeliveryState::kSpeaking",
            alarm_repeat,
        )
        self.assertIn("GetDeviceState() != kDeviceStateSpeaking", alarm_repeat)
        self.assertNotIn("ReminderDeliveryState::kInactive", alarm_repeat)

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
