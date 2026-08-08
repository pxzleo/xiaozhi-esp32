#ifndef _APPLICATION_H_
#define _APPLICATION_H_

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <esp_timer.h>

#include <string>
#include <mutex>
#include <deque>
#include <memory>
#include <functional>
#include <atomic>
#include <optional>

#include "protocol.h"
#include "ota.h"
#include "audio_service.h"
#include "device_state.h"
#include "device_state_machine.h"
#include "netease_music_lyrics.h"
#include "schedule/schedule_manager.h"
#include "proactive/proactive_manager.h"

// Main event bits
#define MAIN_EVENT_SCHEDULE             (1 << 0)
#define MAIN_EVENT_SEND_AUDIO           (1 << 1)
#define MAIN_EVENT_WAKE_WORD_DETECTED   (1 << 2)
#define MAIN_EVENT_VAD_CHANGE           (1 << 3)
#define MAIN_EVENT_ERROR                (1 << 4)
#define MAIN_EVENT_ACTIVATION_DONE      (1 << 5)
#define MAIN_EVENT_CLOCK_TICK           (1 << 6)
#define MAIN_EVENT_NETWORK_CONNECTED    (1 << 7)
#define MAIN_EVENT_NETWORK_DISCONNECTED (1 << 8)
#define MAIN_EVENT_TOGGLE_CHAT          (1 << 9)
#define MAIN_EVENT_START_LISTENING      (1 << 10)
#define MAIN_EVENT_STOP_LISTENING       (1 << 11)
#define MAIN_EVENT_STATE_CHANGED        (1 << 12)
#define MAIN_EVENT_PLAYBACK_DRAINED     (1 << 13)
#define MAIN_EVENT_BARGE_IN_DETECTED    (1 << 14)


enum AecMode {
    kAecOff,
    kAecOnDeviceSide,
    kAecOnServerSide,
};

class Application {
public:
    static Application& GetInstance() {
        static Application instance;
        return instance;
    }
    // Delete copy constructor and assignment operator
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    /**
     * Initialize the application
     * This sets up display, audio, network callbacks, etc.
     * Network connection starts asynchronously.
     */
    void Initialize();

    /**
     * Run the main event loop
     * This function runs in the main task and never returns.
     * It handles all events including network, state changes, and user interactions.
     */
    void Run();

    DeviceState GetDeviceState() const { return state_machine_.GetState(); }
    bool IsVoiceDetected() const { return audio_service_.IsVoiceDetected(); }
    
    /**
     * Request state transition
     * Returns true if transition was successful
     */
    bool SetDeviceState(DeviceState state);

    /**
     * Schedule a callback to be executed in the main task
     */
    void Schedule(std::function<void()>&& callback);

    /**
     * Alert with status, message, emotion and optional sound
     */
    void Alert(const char* status, const char* message, const char* emotion = "", const std::string_view& sound = "");
    void DismissAlert();

    void AbortSpeaking(AbortReason reason);

    /**
     * Toggle chat state (event-based, thread-safe)
     * Sends MAIN_EVENT_TOGGLE_CHAT to be handled in Run()
     */
    void ToggleChatState();

    /**
     * Start listening (event-based, thread-safe)
     * Sends MAIN_EVENT_START_LISTENING to be handled in Run()
     */
    void StartListening();

    /**
     * Stop listening (event-based, thread-safe)
     * Sends MAIN_EVENT_STOP_LISTENING to be handled in Run()
     */
    void StopListening();

    void Reboot();
    void WakeWordInvoke(const std::string& wake_word);
    bool UpgradeFirmware(const std::string& url, const std::string& version = "");
    bool CanEnterSleepMode();
    void SendMcpMessage(const std::string& payload);
    void RegisterMcpBroadcastCallback(std::function<void(const std::string&)> callback);
    void SetAecMode(AecMode mode);
    AecMode GetAecMode() const { return aec_mode_; }
    void PlaySound(const std::string_view& sound);
    AudioService& GetAudioService() { return audio_service_; }
    std::string CreateSchedule(const std::string& kind, const std::string& repeat,
                               const std::string& label, const std::string& trigger_at,
                               int delay_seconds, const std::string& weekdays,
                               const std::string& sections, const std::string& location);
    std::string ListSchedules(const std::string& kind) const;
    std::string DeleteSchedule(uint32_t id);
    std::string ClearSchedules(const std::string& kind);
    std::string StopScheduleAlert();
    bool TryStopScheduleAlert();
    std::string SnoozeScheduleAlert(int minutes);
    std::string ConfigureProactive(const std::string& mode, int daily_limit,
                                   const std::string& quiet_start,
                                   const std::string& quiet_end);
    std::string ProactiveStatus();
    std::string MuteProactiveToday(const std::string& scope);
    std::string AllowProactiveTopic(const std::string& topic);
    std::string BlockProactiveTopic(const std::string& topic);
    std::string CompleteRecentSchedule();
    std::string FollowUpRecentSchedule(int minutes);
    std::string DismissScheduleFollowUp();
    bool IsScheduleAlertActive() const { return schedule_alert_active_.load(); }
    
    /**
     * Reset protocol resources (thread-safe)
     * Can be called from any task to release resources allocated after network connected
     * This includes closing audio channel, resetting protocol and ota objects
     */
    void ResetProtocol();

private:
    Application();
    ~Application();

    std::mutex mutex_;
    std::deque<std::function<void()>> main_tasks_;
    std::unique_ptr<Protocol> protocol_;
    EventGroupHandle_t event_group_ = nullptr;
    esp_timer_handle_t clock_timer_handle_ = nullptr;
    DeviceStateMachine state_machine_;
    ListeningMode listening_mode_ = kListeningModeAutoStop;
    AecMode aec_mode_ = kAecOff;
    std::string last_error_message_;
    AudioService audio_service_;
    netease_music::LyricsTimeline netease_lyrics_;
    std::unique_ptr<Ota> ota_;
    schedule::Manager schedule_manager_;
    schedule::AlertQueue schedule_alert_queue_;
    schedule::ReminderDeliverySequence reminder_delivery_;
    schedule::Task active_schedule_task_;
    proactive::Manager proactive_manager_;
    proactive::FollowUpStore schedule_follow_ups_;
    proactive::HealthTracker health_tracker_;
    proactive::DurableQueue proactive_queue_;
    std::optional<proactive::Event> pending_proactive_event_;
    std::deque<proactive::Event> pending_health_events_;
    std::atomic<bool> schedule_alert_active_{false};
    int64_t schedule_alert_deadline_us_ = 0;
    int64_t schedule_reminder_tts_deadline_us_ = 0;
    int schedule_saved_volume_ = -1;
    uint32_t schedule_volume_revision_ = 0;

    std::function<void(const std::string&)> mcp_broadcast_callback_;

    std::atomic<bool> has_server_time_{false};
    std::atomic<bool> aborted_{false};
    std::atomic<bool> barge_in_detection_active_{false};
    bool assets_version_checked_ = false;
    bool play_popup_on_listening_ = false;  // Flag to play popup sound after state changes to listening
    bool pending_listening_start_ = false;  // Waiting for playback to drain before starting listening (auto mode)
    int clock_ticks_ = 0;
    int64_t uptime_ticks_ = 0;
    std::mutex network_health_mutex_;
    std::deque<int64_t> network_disconnect_us_;
    std::atomic<bool> network_connected_{false};
    bool time_unsynced_health_reported_ = false;
    proactive::RetryBackoff proactive_retry_backoff_;
    proactive::RetryBackoff proactive_save_backoff_;
    proactive::RetryBackoff follow_up_enqueue_backoff_;
    proactive::RetryBackoff health_enqueue_backoff_;
    bool proactive_save_pending_ = false;
    std::atomic<bool> proactive_connection_running_{false};
    std::atomic<bool> proactive_shutdown_{false};
    TaskHandle_t proactive_connection_task_handle_ = nullptr;
    bool proactive_reset_pending_ = false;
    uint32_t proactive_protocol_generation_ = 0;
    TaskHandle_t activation_task_handle_ = nullptr;


    // Event handlers
    void HandleStateChangedEvent();
    void HandleToggleChatEvent();
    void HandleStartListeningEvent();
    void HandleStopListeningEvent();
    void HandleNetworkConnectedEvent();
    void HandleNetworkDisconnectedEvent();
    void HandleActivationDoneEvent();
    void HandleWakeWordDetectedEvent();
    void ContinueOpenAudioChannel(ListeningMode mode);
    void BeginWakeWordInvoke(const std::string& wake_word);
    void ContinueWakeWordInvoke(const std::string& wake_word);
    void StartListeningAudio();
    void ConfigureWakeWordForListening();
    void LoadSchedules();
    void SaveSchedules() const;
    void LoadProactive();
    void SaveProactive() const;
    bool TrySaveProactive(const char* context, bool force = false);
    void CheckProactiveEvents();
    void QueueHealthEvent(const proactive::HealthEvent& event);
    bool SendProactiveEvent(const proactive::Event& event);
    std::string FindFollowUpLabel(uint32_t source_id) const;
    void RecordProactiveSendResult(bool success);
    bool StartProactiveConnectionWorker();
    void ProactiveConnectionTask(uint32_t protocol_generation);
    void CheckSchedules();
    void StartNextScheduleAlert();
    void ShowScheduleAlertPage();
    void FinishScheduleAlert(bool reset_decoder = true, bool reset_delivery = true);
    void RestoreScheduleAlertVolume();
    void RestoreScheduleAlertVolumeAfterDelivery();
    bool NotifyReminderTriggered(const schedule::Task& task, std::time_t now);

    // Activation task (runs in background)
    void ActivationTask();

    // Helper methods
    void CheckAssetsVersion();
    void CheckNewVersion();
    void InitializeProtocol();
    bool HandleNeteaseLyricsNotification(const cJSON* payload);
    void ShowActivationCode(const std::string& code, const std::string& message);
    void SetListeningMode(ListeningMode mode);
    ListeningMode GetDefaultListeningMode() const;
    
    // State change handler called by state machine
    void OnStateChanged(DeviceState old_state, DeviceState new_state);
};


class TaskPriorityReset {
public:
    TaskPriorityReset(BaseType_t priority) {
        original_priority_ = uxTaskPriorityGet(NULL);
        vTaskPrioritySet(NULL, priority);
    }
    ~TaskPriorityReset() {
        vTaskPrioritySet(NULL, original_priority_);
    }

private:
    BaseType_t original_priority_;
};

#endif // _APPLICATION_H_
