# 2026-09-14 · play_wav_url/play_pcm_url 改常驻任务，根治 ESP_ERR_NO_MEM

承接 `2026-09-14-device-hub-production-deploy.md` 里记录的遗留问题：
`play_lan_audio` 真机验证时偶发 `stage=wav_start error=ESP_ERR_NO_MEM`。用户看完
根因分析后明确要求"治本"——把 `play_wav_url` 改成常驻任务模式，而不是加重试兜底。

## 根因（复述，代码定位）

`audio_player.cpp` 的 `play_wav_url()`/`play_pcm_url()` 每次调用都
`xTaskCreate(..., 8192, ...)` 现建一个任务，播放结束后任务自删。这个 8KB 栈必须是
一整块连续的内部 RAM，而这台设备常驻跑着 AEC 全双工管线，内部RAM长期碎片化
（`docs/aec/SUMMARY.md` 记录的基线 `largest_internal ≈ 20480`）。`xTaskCreate` 那
一瞬间如果撞上别的子系统（MQTT/WS/心跳HTTP）的临时占用，连续块跌破 8KB 就失败，
返回 `ESP_ERR_NO_MEM`——不是网络问题，失败发生在任何网络I/O之前。

`docs/aec/SUMMARY.md` 的"已修缺陷"表格第3项记过同一类问题（`far_end` 每次建任务→
`no_task`），修法是"改常驻任务 + 片段缓存"。这次是**同一个反模式第三次出现**在
这个仓库（far_end 是第二次，`play_wav_url`/`play_pcm_url` 是第三次），照搬同一
个修法。

## 实现

参考本仓库已有的常驻任务范式（`playback.c` 的 `playback_task`，AFE全双工那套TTS
流播放引擎），照着同样的思路改 `AudioPlayer`：

- 常驻任务在 `AudioPlayer` 构造函数里建一次（`app_main()` 里这个对象在 WiFi 连上
  之后、`afe_pipeline_init()` 抢占内部RAM之前构造——这个时机是内部RAM碎片化最轻的
  窗口，8KB栈在这时候申请最稳）。
- 新增 `SemaphoreHandle_t request_sem_`（二值信号量）+ `std::atomic<bool> busy_`。
- `play_wav_url()`/`play_pcm_url()` 不再 `xTaskCreate`，改成
  `busy_.compare_exchange_strong(false, true)` 占忙位（原子操作，防并发调用竞态，
  比原来裸指针判空更严谨）→ 填 `pending_*` 字段 → `xSemaphoreGive()` 叫醒常驻任务。
  两个函数都不再可能返回 `ESP_ERR_NO_MEM`。
- `task_entry()` 变成 `for(;;) { xSemaphoreTake(); play_task(); }` 的常驻循环；
  `play_task()` 本体几乎不变，只是把结尾的 `task_ = nullptr` 换成 `busy_ = false`。
- `is_playing()` 从 `task_ != nullptr` 改成 `busy_.load()`；`stop()` 同理改判
  `busy_`。外部调用方（`app_main.cpp` 的 `handle_command`、`mic_asr_button_task`
  等 7 处）**全部只用公开的 `is_playing()` 访问器，一处都不用改**。
- `pending_url_` 等字段的写(调用线程)/读(常驻任务线程)顺序，靠
  `xSemaphoreGive`/`xSemaphoreTake` 的 happens-before 语义保证可见性，跟原来靠
  "任务创建"当同步点是同一个安全级别，没有引入新的数据竞争。

## 验证

编译走 GitHub Actions（本机 IDF 是 5.3.3，工程要求 >=5.4，本地编译结果不可信，
这是 Codex 09-13 已经踩过、记录过的坑，这次直接绕开没有重踩）。

- tag `ota-esp32-wangyutang-v37-persistent-audio-task`（commit `396d090`），
  release `rel-aa0dc3743beb`，2449600 字节。
- OTA job `ota-7fa56b82ffbe`，`v36-heartbeat-fix` → `v37-persistent-audio-task`，
  38 秒内 `verified`。
- **压力测试**：4 轮连续 `play_lan_audio` 调用，全部 `status: done`，**0 失败**。
  关键的是测试时 `largest_internal` 一度跌到 **7680 字节**——比原来触发失败的
  21504 更紧张，甚至低于旧代码需要的 8192 字节栈本身。换成旧代码，这两轮
  （round 1、round 3）几乎必然复现 `ESP_ERR_NO_MEM`；新代码因为不再需要现场分配，
  全部成功。
- 内存曲线：`free_internal` 30555→30507→19415→18859→30419→20787，**不是单调下降**
  （round 2→3 之间从 19415 回升到 30419），排除了新方案本身（信号量+原子量）引入
  持续泄漏的可能。
- 每轮都交叉核对 Spark 静态服务器的访问日志，GET 时间点与命令完成时间对应。

## 现状

- 代码已合并、编译、OTA、真机压测通过，**`ESP_ERR_NO_MEM` 这个失败模式已经从
  结构上消除**，不是靠重试掩盖。
- `play_pcm_url`（`stream_prepare` 命令用）复用同一套改动，逻辑同构，但本次压测
  只覆盖了 `play_wav_url`/`play_lan_audio` 路径，`stream_prepare` 没有单独跑一次
  真机验证——两者共享同一个常驻任务和同一套 busy_/信号量逻辑，理论上同样受益，
  但严格说这一条是"代码审查确认同构"而不是"独立实测"，值得记录成一个还没关闭
  的验证缺口。
