# 设备主动助理每日简报（v1）

每日简报复用设备本地 `schedule::Manager`。设备是时间和重复规则的权威来源，NVS 保存任务；到点后发送 `notifications/assistant/triggered`，服务端获取天气/新闻并用 TTS 播报。

- `self.schedule.create` 使用 `kind=briefing`；`sections` 只能是 `weather`、`news` 或 `weather,news`，包含天气时 `location` 必须是明确的 1 到 40 个 Unicode 字符。
- 时间、重复规则和 `weekdays` 与闹铃/提醒一致。日期、时间、重复方式或地点有歧义时必须追问，不能猜测。
- `list/delete/clear/stop` 支持简报；简报不支持 `snooze`。
- 设备不保存或发送任意提示词、URL、服务端工具名。重复任务重启后跳过已错过播报，单次任务沿用 5 分钟恢复窗口。
- 等待服务端 TTS 最多 30 秒；用户停止或打断时终止当前交付。
- 若每日简报打断了正在播放的网易云歌曲，服务端会在简报 TTS 完整结束后恢复原队列和原歌曲；当前从该歌曲开头重新播放。用户在简报期间插话、停止音乐或发起新播放时不恢复。普通闹铃和提醒仍不恢复音乐。
- 通用提醒页兼容 1.32 AMOLED 和 1.85C LCD；最终烧录仍需明确目标板型。

示例：“工作日早上八点播报广州天气和今天新闻”创建 `repeat=weekdays`、`sections=weather,news`、`location=广州` 的任务。
