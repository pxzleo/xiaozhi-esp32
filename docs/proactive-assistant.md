# 设备主动助理每日简报（v1）

每日简报复用设备本地 `schedule::Manager`。设备是时间和重复规则的权威来源，NVS 保存任务；到点后发送 `notifications/assistant/triggered`，服务端获取天气/新闻并用 TTS 播报。

- `self.schedule.create` 使用 `kind=briefing`；`sections` 只能是 `weather`、`news` 或 `weather,news`，包含天气时 `location` 必须是明确的 1 到 40 个 Unicode 字符。
- 时间、重复规则和 `weekdays` 与闹铃/提醒一致。日期、时间、重复方式或地点有歧义时必须追问，不能猜测。
- `list/delete/clear/stop` 支持简报；简报不支持 `snooze`。
- 设备不保存或发送任意提示词、URL、服务端工具名。重复任务重启后跳过已错过播报，单次任务沿用 5 分钟恢复窗口。
- 等待服务端 TTS 最多 30 秒；用户停止或打断时终止当前交付。
- 若每日简报打断了正在播放的网易云歌曲，服务端会在简报 TTS 完整结束后恢复原队列和原歌曲；当前从该歌曲开头重新播放。用户在简报期间插话、停止音乐或发起新播放时不恢复。普通闹铃和提醒仍不恢复音乐。
- 最终目标板为 Waveshare ESP32-S3 Touch LCD 1.85C V2.0；已使用 ESP-IDF 6.0.1 完成该目标的完整固件编译。通用提醒页仍保持与 1.32 AMOLED 的代码兼容。

示例：“工作日早上八点播报广州天气和今天新闻”创建 `repeat=weekdays`、`sections=weather,news`、`location=广州` 的任务。

## 积极主动模式（v2）

- 设备以独立 `proactive::Manager` 作为主动策略权威。配置和运行状态保存在 NVS `proactive` 命名空间；默认 `aggressive`、普通事件每日最多 5 次。`active` 默认 3 次，`conservative` 只允许闹铃、明确提醒和 critical 健康事件。`today_silent` 在本地日期变化后恢复此前模式。
- 安静时段默认不存在，只有用户明确同时提供 `quiet_start/quiet_end` 后才生效；支持跨午夜。策略还执行 topic allow/block、同类 30 分钟冷却和每日预算：allow 集合非空时作为白名单，普通事件只有 topic 在集合内才允许；critical 事件不受白名单限制。闹铃、明确提醒及 critical 健康事件不受普通预算阻止。系统时间未同步时不重置日期、不恢复 `today_silent`，也不触发非关键主动事件。
- 统一主动事件至少包含 `event_id/topic/priority/reason/created_at/expires_at/dedupe_key/requires_response`。需跨断线的 follow-up 与健康事件进入 NVS 持久队列，按优先级和创建时间取出；普通建议过期即丢弃，明确闹铃/提醒仍走原调度队列并优先。
- 普通提醒 TTS 真正 stop 后，策略允许时持久化 10 分钟后的完成确认；用于“最近”判定的 `source_triggered_at` 始终取原任务权威 `trigger_at`，不使用 TTS stop 时刻。每项最多主动追问一次。闹铃、每日简报和 follow-up 本身不会递归创建追问。断电恢复后未过期项继续；到期超过 5 分钟仍未发出的项丢弃。`complete_recent/follow_up/dismiss_follow_up` 只处理最近、未过期且唯一的候选，原始触发时刻相同时明确报错。
- 主动模式工具为 `self.proactive.configure/status/mute/allow_topic/block_topic`。配置、主题和完成追踪工具都应直接调用，不在工具前播报“我来处理一下”。
- 同类型、同内容、同一时刻但不同日期的单次闹铃或提醒同时存在时，第二次创建成功后建议用户改成重复任务。
- “晚点、有空、回头做某事”等表达如果没有明确提醒意图或时间，主对话必须先询问是否需要提醒及具体时间，不能直接创建任务。

## 设备健康通知

设备健康通知统一使用 `notifications/device/health` version 1，携带 `event_id`、`kind`、`severity`（`info/warning/critical`）、`occurred_at`、受控 `details`，并同时携带统一主动事件字段。首次接入：5 分钟内网络断开至少 3 次、启动 10 分钟后仍未校时、Opus 解码失败/解码器不可用、检测到 OTA 新版本；恢复事件复用相同 `dedupe_key` 并带 `recovered=true`。活动状态与待发送通知持久化去重；通道不可用时先显示本地提示，保留队列供后续连接上报。

网络抖动只统计 `Connected → Disconnected` 的真实转换，扫描和初始未连接状态不计数；转换时用单调时钟记录，事件位合并不会丢失次数。健康提示仅在设备空闲且本地播放队列空闲时播放 popup 音，不中断闹铃、提醒或正在播报的内容。恢复事件替换相同 dedupe key 的未发送故障事件，并绕过普通 mode、quiet、预算和冷却，确保恢复最终上报。

离线主动事件使用 5 秒起步、最长 5 分钟的指数退避。主动队列只在既有音频通道已开启时发送，未开启时依赖现有异步会话连接流程并保留队列，绝不从主任务同步调用最长可等待 10 秒的 `OpenAudioChannel`。NVS 主动状态采用每片 1800 字节、总计最多 3600 字节的明确预算，最多保存 4 项完成追踪、8 项队列事件和 8 项主题规则，并在写入前用 NVS entry 统计保留 16 个安全 entry，避免占满目标板仅 16KB 的共享 NVS 分区。保存使用可返回错误的直接 NVS 操作，不经过会 `ESP_ERROR_CHECK` 复位的 Settings 写路径；内部事件保存失败会记录上下文、显示本地错误并按退避重试，MCP 配置和完成操作则回滚内存并返回明确失败。超限不会扩大分区或伪造文件系统可用空间；该 entry 预检只服务于主动状态原子保存，不等同于跨目标板的 `storage_low` 指标。

当前仓库没有跨目标板一致且可靠的“剩余可写存储空间”API，因此未接入 `storage_low`，不以堆内存或分区总大小伪造该指标。AudioService 会保存首次解码器初始化失败状态和错误码，健康回调注册后立即补报；运行期重建或实际解码失败同样上报，后续成功创建或成功解码会发送 recovered。
