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

- 设备以独立 `proactive::Manager` 作为主动策略权威。配置和运行状态保存在 NVS `proactive` 命名空间；默认 `aggressive`，其普通主动发言不受每日总额度限制，且不能配置成有限次数。`active` 默认每日 5 次，可在 1 至 5 次内调低；`conservative` 固定为 1，但策略仍只允许闹铃、明确提醒和 critical 健康事件。`today_silent` 的当前额度为 0，并在本地日期变化后恢复此前模式及此前额度。
- 安静时段默认不存在，只有用户明确同时提供 `quiet_start/quiet_end` 后才生效；支持跨午夜。策略还执行 topic allow/block、同类 30 分钟冷却和每日预算；`aggressive` 仅绕过每日总额度，安静时段、主题规则、同主题冷却、过期、连接与投递安全规则仍照常执行。主题只允许 `reminder/calendar/weather/news/music/health/habit/system`，设备会同时保留并持久化全部 8 个策略主题的有效冷却记录，避免无限模式下因主题轮换提前淘汰冷却；内部 `follow_up` 按 `reminder`、`health_critical` 按 `health` 应用规则。allow 集合非空时作为白名单，普通事件只有 topic 在集合内才允许；critical 事件不受白名单限制。闹铃、明确提醒及 critical 健康事件不受普通预算阻止。系统时间未同步时不重置日期、不恢复 `today_silent`，也不触发非关键主动事件。
- 统一主动事件至少包含 `event_id/topic/priority/reason/created_at/expires_at/dedupe_key/requires_response`。设备内部仍使用 OTA 校准后的本地墙钟调度；事件首次获得有效服务端时间时扣除 `timezone_offset`，把真实 UTC `protocol_created_at/protocol_expires_at` 随事件一并持久化，后续发送 `created_at/expires_at/occurred_at` 时直接使用，不能因重启或时区变化再次换算。尚未校时且没有持久 UTC 时间的事件不得发送；校时完成后以首个可靠时刻补齐。需跨断线的 follow-up 与健康事件进入 NVS 持久队列，按优先级和创建时间取出；普通建议过期即丢弃，明确闹铃/提醒仍走原调度队列并优先。
- 普通提醒 TTS 真正 stop 后，策略允许时持久化 10 分钟后的完成确认；用于“最近”判定的 `source_triggered_at` 始终取原任务权威 `trigger_at`，不使用 TTS stop 时刻。每项最多主动追问一次。闹铃、每日简报和 follow-up 本身不会递归创建追问。断电恢复后未过期项继续；到期超过 5 分钟仍未发出的项丢弃。`complete_recent/follow_up/dismiss_follow_up` 只处理最近、未过期且唯一的候选，原始触发时刻相同时明确报错。
- 主动模式工具为 `self.proactive.configure/status/mute/allow_topic/block_topic`。配置、主题和完成追踪工具都应直接调用，不在工具前播报“我来处理一下”。
- 同类型、同内容、同一时刻但不同日期的单次闹铃或提醒同时存在时，第二次创建成功后建议用户改成重复任务。
- “晚点、有空、回头做某事”等表达如果没有明确提醒意图或时间，主对话必须先询问是否需要提醒及具体时间，不能直接创建任务。

## 设备健康通知

设备健康通知统一使用 `notifications/device/health` version 1，携带 `event_id`、`kind`、`severity`（`info/warning/critical`）、`occurred_at`、受控 `details`，并同时携带统一主动事件字段。首次接入：5 分钟内网络断开至少 3 次、启动 10 分钟后仍未校时、Opus 解码失败/解码器不可用、检测到 OTA 新版本；恢复事件复用相同 `dedupe_key` 并带 `recovered=true`。活动状态与待发送通知持久化去重；通道不可用时先显示本地提示，保留队列供后续连接上报。

网络抖动只统计 `Connected → Disconnected` 的真实转换，扫描和初始未连接状态不计数；转换时用单调时钟记录，事件位合并不会丢失次数。健康提示仅在设备空闲且本地播放队列空闲时播放 popup 音，不中断闹铃、提醒或正在播报的内容。健康事件保留严重级别映射：info 为 normal、warning 为 high、critical 为 critical，只有 critical health 绕过普通 mode、quiet、预算和冷却；恢复通知走独立旁路。设备只接受 `network_flapping`、`time_unsynchronized`、`ota_update_available`、`audio_decode_failed` 四类健康事件，未知 kind 明确拒绝。四类事件的活动和恢复共用 dedupe key，在唯一的 8 项持久主队列内直接替换；同优先级下健康事件优先于普通事件，队列满时健康事件可淘汰非 critical 的非健康事件，因此不再维护会重复占用 NVS 的健康溢出层。

离线主动事件使用 5 秒起步、最长 5 分钟的指数退避。通道未开启时，单一 FreeRTOS worker 在确认网络已连接、设备空闲且无其他连接 worker 后执行 `OpenAudioChannel`。独立原子 `busy` 是跨任务协议门禁与析构等待的唯一权威：主线程在创建 worker 前发布 true，创建失败才回滚；worker 全程保持 true，并以二值信号量 Give 作为最后一次 Application 访问，返回后仅由任务入口执行 `vTaskDelete`。Finish 取得完成信号后才由主线程清空 handle 和 busy；析构看到 busy=true 就无条件等待。连接期间的 start/stop/toggle/具体唤醒词等边沿操作进入主线程拥有的 16 项 FIFO，按原顺序逐项重放；队列满会明确日志和本地提示，不静默合并。

主动状态的合法 JSON 上限为 7200 字节，保存为单个 `state` blob；生产与主机测试共用有界长窗口 LZSS 编解码器，压缩 blob 上限 3600 字节，并携带原始长度和 FNV-1a 校验，解压上限仍为 7200 字节。持久队列只接受设备实际产生的 `follow_up` 与四类 health schema，并限制标识、原因、受控 metadata 键和值长度；任意外观相似事件不会进入 NVS。单键 commit 是断电一致性边界，不假设多 key 原子事务；旧分片只在新 blob 成功提交后清理，损坏 blob 会删除并提交隔离，避免每次启动重复失败。统一队列始终最多 8 项，正在播放的事件使用独立可空 `pending` 字段；迁移旧版 9 项数组时会把唯一的最高优先/最新追问拆入 pending，同优先同时间候选明确报歧义。容量预检按 blob 的旧、新双版本峰值计算：3600 字节按 8 个元数据 entry 加每 32 字节一个 entry，并额外保留 16 entry 安全余量；16KB 目标模型在首次可用 378 entry、旧 blob 存在且全局可用约 142 entry 时均可重复写入，其他命名空间真实挤占过高仍明确拒绝。统计、压缩、写入或提交失败会日志告警、保留内存状态并退避重试。

当前仓库没有跨目标板一致且可靠的“剩余可写存储空间”API，因此未接入 `storage_low`，不以堆内存或分区总大小伪造该指标。AudioService 会保存首次解码器初始化失败状态和错误码，健康回调注册后立即补报；运行期重建或实际解码失败同样上报，后续成功创建或成功解码会发送 recovered。

## 服务端外界监测探测（v3）

- 设备完成激活并获得可靠服务端时间后，按设备 MAC 的固定散列在 0 至 30 秒内错峰启动探测；之后空闲时每 5 分钟使用设备鉴权请求 `GET /device/proactive/pending`。请求沿用 manager-api 基址、`Device-Id`、`Client-Id` 与 WebSocket Bearer 令牌，不记录令牌。失败从 5 分钟开始指数退避，最多 30 分钟；成功空结果采用服务端 `retry_after_seconds`，当前为 300 秒。监测开关、城市、来源和存量默认值都由服务端控制，固件不保存第二套配置。
- manager-api 正常返回带准确非零 `Content-Length` 的 UTF-8 JSON。设备仍兼容中间代理改写出的 `Transfer-Encoding: chunked` 或连接关闭定界响应：声明长度时必须精确读满，未声明长度时读到 HTTP 客户端报告正文结束；两种路径都严格限制正文最多 2048 字节，空正文、截断、读取失败或超限均明确失败并进入探测退避。
- 安全信封只接受 `pending/event_id/topic/priority/created_at/expires_at/retry_after_seconds`；topic 仅允许 `weather/news`，事件 ID、优先级、时间范围和过期状态都要校验。信封不包含且设备不请求播报文本、事实、新闻链接或事件类型；非法或已过期事件只写受控日志并丢弃。
- 空结果绝不建立音频 WebSocket。有待播事件且设备空闲、没有闹铃/提醒、本地主动事件、播放、录音或其他连接 worker 时，复用单一主动建链 worker。连接成功后设备只发送 `notifications/assistant/external_triggered`：

```json
{
  "jsonrpc": "2.0",
  "method": "notifications/assistant/external_triggered",
  "params": {"version": 1, "event_id": "evt-...", "speak": true}
}
```

- 设备发送通知不代表已领取或已投递；manager-api 的 180 秒 claim、服务端 TTS 完成信号和审计终态仍是唯一权威。连接或发送失败后保留待播 ID 并退避重试，过期即丢弃。用户操作、手动连接、播放和录音始终优先，打断沿用现有语音通道；新闻播报后的收听与天气播报后的结束均由服务端决定。
