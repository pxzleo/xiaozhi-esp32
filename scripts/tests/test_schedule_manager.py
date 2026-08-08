import json
import shutil
import subprocess
import tempfile
import unittest
from collections import deque
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


class ScheduleManagerTest(unittest.TestCase):
    def test_deferred_edge_actions_preserve_fifo(self):
        actions = deque()
        expected = ["stop", "start", "toggle", "toggle", "wake:first", "wake:second"]
        for action in expected:
            actions.append(action)
        replayed = []
        while actions:
            replayed.append(actions.popleft())
        self.assertEqual(replayed, expected)

    def test_proactive_nvs_replacement_peak_model(self):
        safety_entries = 16

        def entry_cost(compressed_bytes):
            return 8 + (compressed_bytes + 31) // 32

        def can_replace(global_available, proactive_used, compressed_bytes):
            required = entry_cost(compressed_bytes)
            return (global_available + proactive_used >=
                    required + proactive_used + safety_entries)

        max_required = entry_cost(3600)
        half_required = entry_cost(1800)
        self.assertTrue(can_replace(378, 0, 3600))  # 16KB target first save.
        self.assertTrue(can_replace(142, max_required, 3600))  # Same-size rewrite.
        self.assertTrue(can_replace(142, max_required, 1800))  # Shrink rewrite.
        self.assertTrue(can_replace(142, half_required, 3600))  # Grow back to max.
        self.assertFalse(can_replace(100, max_required, 3600))  # Other namespaces full.

    def test_proactive_state_serializes_and_restores_previous_daily_limit(self):
        application = (ROOT / "main" / "application.cc").read_text(encoding="utf-8")
        self.assertIn('cJSON_AddNumberToObject(json, "previous_daily_limit"', application)
        self.assertIn('cJSON_GetObjectItem(config_json, "previous_daily_limit")', application)
        self.assertIn('previous_daily_limit != nullptr ?', application)

    def test_maximum_proactive_state_fits_persistence_budget(self):
        topics = ["reminder", "calendar", "weather", "music", "health", "habit", "system"]
        config = {
            "mode": "aggressive",
            "mode_before_silent": "aggressive",
            "previous_daily_limit": 0,
            "silent_date": 20260808,
            "daily_limit": 0,
            "quiet_start": 1439,
            "quiet_end": 1439,
            "allowed_topics": topics[:4],
            "blocked_topics": topics[4:],
            "budget_date": 20260808,
            "delivered_today": 6,
            "last_delivered": {topics[i]: 1786159999 for i in range(7)},
        }
        follow_ups = [
            {
                "source_id": source_index + 1,
                "label": "".join(chr(0x1f300 + char_index + source_index * 80)
                                 for char_index in range(80)),
                "source_triggered_at": 1786159000 + source_index,
                "due_at": 1786159600 + source_index,
                "expires_at": 1786159900 + source_index,
                "asked": True,
            }
            for source_index in range(4)
        ]
        kinds = ("network_flapping", "time_unsynchronized",
                 "ota_update_available", "audio_decode_failed")
        health = {
            kind: {
                "active": True,
                "last_changed_at": 1786159000 + index,
                "dedupe_key": f"health:{kind}",
            }
            for index, kind in enumerate(kinds)
        }

        def event(event_id, event_topic, priority, dedupe_key, metadata):
            return {
                "event_id": event_id,
                "topic": event_topic,
                "priority": priority,
                "reason": ("schedule follow up" if event_topic == "follow_up"
                           else "device health recovered" if metadata.get("recovered") == "true"
                           else "device health"),
                "created_at": 1786159000,
                "expires_at": 1786245400,
                "dedupe_key": dedupe_key,
                "requires_response": "source_id" in metadata,
                "metadata": metadata,
            }

        queue = [
            event(f"{kind}:recovered:1786159000", "health", "normal",
                  f"health:{kind}", {
                      "health_kind": kind,
                      "severity": "info",
                      "recovered": "true",
                      "detail.error_code": "-2147483648",
                      "detail.version": "v" + "9" * 63,
                  })
            for kind in kinds
        ]
        queue += [event(f"follow-up-{i + 1}-1786159600", "follow_up", "normal",
                        f"follow-up:{i + 1}", {"source_id": str(i + 1)})
                  for i in range(4)]
        pending = event("follow-up-pending-1786159600", "follow_up", "normal",
                        "follow-up:pending", {"source_id": "4", "cue_played": "true"})
        state = {"config": config, "follow_ups": follow_ups,
                 "health": health, "queue": queue, "pending": pending}
        encoded = json.dumps(state, ensure_ascii=False, separators=(",", ":")).encode()

        def production_lzss(data):
            output = bytearray(b"PZ2\0" + len(data).to_bytes(4, "little") + b"\0\0\0\0")
            checksum = 2166136261
            for value in data:
                checksum = ((checksum ^ value) * 16777619) & 0xffffffff
            output[8:12] = checksum.to_bytes(4, "little")
            cursor = 0
            while cursor < len(data):
                flags_index = len(output)
                output.append(0)
                for bit in range(8):
                    if cursor >= len(data):
                        break
                    best_length = best_offset = 0
                    for candidate in range(max(0, cursor - 65535), cursor):
                        length = 0
                        while (length < 255 and cursor + length < len(data) and
                               data[candidate + length] == data[cursor + length]):
                            length += 1
                        if length >= 4 and length > best_length:
                            best_length, best_offset = length, cursor - candidate
                    if best_length >= 4:
                        output[flags_index] |= 1 << bit
                        output.extend((best_offset & 0xff, best_offset >> 8, best_length))
                        cursor += best_length
                    else:
                        output.append(data[cursor])
                        cursor += 1
            return bytes(output)

        compressed = production_lzss(encoded)
        self.assertGreater(len(encoded), 3600)
        self.assertLess(len(encoded), 7200)
        self.assertLessEqual(len(compressed), 3600)
        self.assertLessEqual(2 * (8 + (len(compressed) + 31) // 32) + 16, 378)

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
        application_h = (ROOT / "main" / "application.h").read_text(encoding="utf-8")
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
            "pending_proactive_event_ &&", 1
        )[1].split("if (bits & MAIN_EVENT_TOGGLE_CHAT)", 1)[0]
        self.assertIn("SendProactiveEvent(event)", drained)
        self.assertIn("StartProactiveConnectionWorker()", drained)
        self.assertLess(drained.index("SendProactiveEvent(event)"),
                        drained.index("pending_proactive_event_.reset()"))
        self.assertNotIn("proactive_queue_.Push(event)", drained)
        self.assertNotIn("continue;", drained)
        send_proactive = application.split("bool Application::SendProactiveEvent", 1)[1]
        send_proactive = send_proactive.split("std::string Application::FindFollowUpLabel", 1)[0]
        self.assertIn("protocol_->IsAudioChannelOpened()", send_proactive)
        self.assertNotIn("OpenAudioChannel", send_proactive)
        self.assertIn("PendingDue(now)", proactive_check)
        self.assertLess(
            proactive_check.index("proactive_queue_.Push(event)"),
            proactive_check.index("schedule_follow_ups_.MarkAsked"),
        )
        self.assertLess(
            proactive_check.index('TrySaveProactive("due follow-up transaction", true)'),
            proactive_check.index("audio_service_.PlaySound(Lang::Sounds::OGG_POPUP)"),
        )
        self.assertIn("StartProactiveConnectionWorker()", proactive_check)
        pending_progress = proactive_check.split("if (pending_proactive_event_", 1)[1]
        pending_progress = pending_progress.split("if (!pending_proactive_event_", 1)[0]
        self.assertIn("StartProactiveConnectionWorker()", pending_progress)
        self.assertIn("MAIN_EVENT_PLAYBACK_DRAINED", pending_progress)
        send_mcp = application.split("void Application::SendMcpMessage", 1)[1]
        send_mcp = send_mcp.split("void Application::SetAecMode", 1)[0]
        self.assertIn("bool accepted = false", send_mcp)
        self.assertIn("accepted && mcp_broadcast_callback_", send_mcp)
        self.assertIn("accepted = protocol_->SendMcpMessage(payload)", send_mcp)
        self.assertIn("消息等待队列已满", send_mcp)
        worker = application.split("void Application::ProactiveConnectionTask", 1)[1]
        worker = worker.split("void Application::CheckProactiveEvents", 1)[0]
        self.assertIn("protocol_->OpenAudioChannel()", worker)
        worker_task = worker.split("void Application::FinishProactiveConnection", 1)[0]
        self.assertNotIn("proactive_connection_busy_.store(false", worker_task)
        self.assertIn('"proactive_conn", 4096 * 2, this, 11', application)
        self.assertLess(worker.index("Schedule([this, shutdown_token"),
                        worker.index("xSemaphoreGive(proactive_connection_done_)"))
        finish = application.split("void Application::FinishProactiveConnection", 1)[1]
        finish = finish.split("void Application::CheckProactiveEvents", 1)[0]
        self.assertLess(
            finish.index("xSemaphoreTake(proactive_connection_done_"),
            finish.index("proactive_connection_task_handle_ = nullptr"),
        )
        self.assertLess(finish.index("proactive_connection_task_handle_ = nullptr"),
                        finish.index("proactive_connection_busy_.store(false"))
        self.assertNotIn("Schedule([this", finish)
        self.assertIn("proactive_finish_pending_ = true", finish)
        destructor = application.split("Application::~Application()", 1)[1]
        destructor = destructor.split("bool Application::SetDeviceState", 1)[0]
        self.assertIn("proactive_connection_busy_.load(std::memory_order_acquire)", destructor)
        self.assertIn("xSemaphoreTake(proactive_connection_done_, portMAX_DELAY)", destructor)
        self.assertIn("deferred_bits | MAIN_EVENT_CLOCK_TICK", finish)
        for method in ("ContinueOpenAudioChannel", "ContinueWakeWordInvoke",
                       "NotifyReminderTriggered"):
            body = application.split(f"Application::{method}", 1)[1]
            self.assertIn("IsProactiveConnectionBusy()", body[:1200])
        self.assertLess(proactive_check.index("IsProactiveConnectionBusy()"),
                        proactive_check.index("protocol_->IsAudioChannelOpened()"))
        reset = application.split("void Application::ResetProtocol()", 1)[1]
        self.assertIn("proactive_reset_pending_ = true", reset)
        self.assertIn("LoadProactive()", application)
        for public_name, handler, event_bit in (
                ("ToggleChatState", "HandleToggleChatEvent", "MAIN_EVENT_TOGGLE_CHAT"),
                ("StartListening", "HandleStartListeningEvent", "MAIN_EVENT_START_LISTENING"),
                ("StopListening", "HandleStopListeningEvent", "MAIN_EVENT_STOP_LISTENING")):
            public_body = application.split(f"void Application::{public_name}()", 1)[1]
            public_body = public_body.split("\n}", 1)[0]
            self.assertIn("Schedule([this]", public_body)
            self.assertIn(handler, public_body)
            self.assertNotIn(f"xEventGroupSetBits(event_group_, {event_bit})", public_body)
        wake_callback = application.split("callbacks.on_wake_word_detected", 1)[1]
        wake_callback = wake_callback.split("callbacks.on_vad_change", 1)[0]
        self.assertIn("Schedule([this, wake_word]", wake_callback)
        self.assertNotIn("MAIN_EVENT_WAKE_WORD_DETECTED", wake_callback)
        self.assertIn('nvs_erase_key(quarantine_handle, "state")', application)
        self.assertIn("nvs_commit(quarantine_handle)", application)
        self.assertIn("migrated_pending", application)
        self.assertIn("loaded_legacy_pz1", application)
        self.assertIn("if (loaded_legacy_pz1) proactive_save_pending_ = true", application)
        self.assertIn("legacy_tail = queued.back()", application)
        self.assertIn("SaveProactive()", application)
        self.assertIn("time_unsynchronized", application)
        self.assertIn("uptime_ticks_ >= 600", application)
        self.assertIn("RemoveByDedupeKey", application)
        self.assertIn("active_schedule_task_.trigger_at, now", application)
        self.assertIn("proactive_retry_backoff_.Ready", application)
        self.assertIn("RecordProactiveSendResult", application)
        self.assertIn("kMaxSerializedBytes = 7200", application)
        self.assertIn("nvs_get_stats(nullptr, &nvs_stats)", application)
        self.assertIn("nvs_get_used_entry_count(handle, &namespace_used_entries)", application)
        self.assertIn("nvs_stats.available_entries + namespace_used_entries", application)
        self.assertIn("required_entries + namespace_used_entries +", application)
        self.assertIn("kNvsSafetyEntries = 16", application)
        self.assertIn("TrySaveProactive", application)
        self.assertIn("proactive_save_pending_ = true", application)
        self.assertIn("未应用修改", application)
        self.assertEqual(application.count("SaveProactive();"), 1)
        self.assertIn("recovered_health ||", application)
        self.assertNotIn("event.priority = proactive::Priority::kCritical", application)
        self.assertNotIn("pending_health_events_", application + application_h)
        health_queue = application.split("void Application::QueueHealthEvent", 1)[1]
        health_queue = health_queue.split("bool Application::SendProactiveEvent", 1)[0]
        self.assertIn("audio_service_.PlaySound(Lang::Sounds::OGG_POPUP)", health_queue)
        self.assertIn("HealthTracker::IsSupportedKind(health.kind)", health_queue)
        self.assertIn("proactive_queue_.Push(event)", health_queue)
        wake_invoke = application.split("void Application::WakeWordInvoke", 1)[1]
        wake_invoke = wake_invoke.split("bool Application::CanEnterSleepMode", 1)[0]
        self.assertLess(wake_invoke.index("Schedule([this, wake_word]"),
                        wake_invoke.index("IsProactiveConnectionBusy()"))
        self.assertIn('DeferProactiveAction("external wake invoke"', wake_invoke)
        self.assertIn("std::deque<std::function<void()>> proactive_deferred_actions_", application_h)
        self.assertIn("kMaxDeferredProactiveActions = 16", application_h)
        finish_fifo = finish.split("while (!proactive_deferred_actions_.empty())", 1)[1]
        self.assertLess(finish_fifo.index("front()"), finish_fifo.index("pop_front()"))
        self.assertLess(finish_fifo.index("pop_front()"), finish_fifo.index("action()"))
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
