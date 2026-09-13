# 2026-09-14 · device_hub 双向分叉合并（本仓库 ↔ 生产），以及 play_lan_audio

## 背景：一次差点造成回归的发现

本来只是要给 `cloud/device_hub/server.py` 加一个新 action `play_lan_audio`。加之前
按 `docs/PROJECT_CONTEXT.md` 的告诫（"云端的运行容器可能存在未回写仓库的热修复…
发布前必须重新核验"）做了一次 diff，结果发现**本仓库这份 `server.py` 跟线上真正
跑着的那份差了 244 行，而且是双向的**——两边都有对方没有的东西。

如果当时不 diff、直接拿本仓库这份重建 device-hub 容器（而 `cloud/device_hub/Dockerfile`
里就写着 `COPY server.py ./server.py`，Codex 加 `COPY ota_manager.py` 那个 commit
`c9036ab` 的目的正是为了让"以后重建容器不会丢掉 OTA 端点"），生产会**当场丢掉**
boot_announce、upload_audio 的 `play` 参数、DISPATCHED_TIMEOUT_S 三项真实功能。

### 一个中途走错的弯路（值得记下来）

第一次分析时我把风险描述成"esp32_wifi 这份副本落后于生产"，并提议"把生产的热修复
回写进来"。用户追问"这个建议是否合理"之后再查，发现**我当时假设错了部署源头**：
按字节数比，`wangyutang/device_hub/server.py` 只跟线上差 17 行，而本仓库这份差 244 行，
于是我改口说"应该把 play_lan_audio 加到 wangyutang 仓库去"。**这个改口是错的**——
用户指出"我们现在维护和更新的 esp32 项目是 workspace/esp32_wifi"，再查证据：

- `cloud/device_hub/Dockerfile` 与 tang 上正在用的那份**逐字节一致**
- `cloud/device_hub/ota_manager.py` 已经在本仓库里（tang 上那份只多 103 字节的热修复）
- `docs/PROJECT_CONTEXT.md` 白纸黑字写着"云端设备控制面：`cloud/device_hub/`"是权威源码
- `static/` 四个文件与容器内**完全一致**

**结论：迁移已经完成了约 80%，`server.py` 是最后一块没对齐的。** 那 244 行不是
"副本不重要"，而是**迁移欠下的债**。所以第一次的直觉（在本仓库合并）是对的，中间
那次改口是因为拿"当前部署源头"当成了"应该往哪儿写"，两者不是一回事。

教训：**"谁是当前部署源头"和"谁应该是权威源码"是两个不同的问题**，把前者的答案
当成后者的答案，会得出方向完全相反的结论。

## 分叉时间线（git log -S 考古）

两个仓库在 2026-08-31 / 09-01 分叉，各自独立演进了两周：

| 日期 | 来源 | 变更 | 生产有 | 本仓库有 |
|---|---|---|---|---|
| 08-31 | wangyutang `7f3451e` | `DISPATCHED_TIMEOUT_S` + 旧 WS 分包(262144/32KB) | ✅ | ❌ |
| 09-01 | esp32_wifi `7c6ccd4` | **20ms 定长分帧**（v7-audio-fix 的服务端一半） | ❌ | ✅ |
| 09-01 | esp32_wifi `7f38844` | OTA `accepted` 立即清 retained 命令 | ❌ | ✅ |
| 09-02 | esp32_wifi `41dba29` | `_device_mqtt_ready()` + `"dispatching"` 占位 | ❌ | ✅ |
| 09-04 | wangyutang `d73a201` | `boot_announce` 功能 | ✅ | ❌ |
| 09-10 | wangyutang `26891e8` | `upload_audio` 的 `play` 参数 | ✅ | ❌ |

**顺带发现**：09-01 那个 20ms 分帧是 `OTA_RELEASE.md` 里记的 v7-audio-fix 的一部分，
修的是"WebSocket/TLS 奇数字节分片造成 16-bit PCM 样本错位噪声"。固件那一半通过 OTA
上线了，**服务端这一半从来没上过生产**——也就是说生产至今跑的是半个修复。

## 合并结果

### 从生产补回本仓库（不补就会在重建容器时丢掉）

1. `DISPATCHED_TIMEOUT_S = 300` 常量 + 后台线程里的自动过期逻辑。
   （这正是本次排查全程看到的 `"timeout: no ACK after 303s"` 的真正来源——
   一开始误以为是某个外部测试工具写的。）
2. `upload_audio` 的 `play` 参数（`play=0` 只存不播）。这是 `docs/aec/SUMMARY.md`
   里"已修缺陷 #1"（AEC 采样上传误触发回放、抢占扬声器）的修复代码。
3. `boot_announce` 整个功能（`_WEEKDAYS`、`BOOT_ANNOUNCE_VOLUME`、`_boot_announce_for()`、
   `BootAnnounceReq`、`/api/boot_announce` 与 `/api/device/{id}/boot_announce` 两个路由）
   + `import datetime`。
4. `ota_manager.py` 的 103 字节热修复：给 `command.status == "done"` 那个 elif 分支加上
   `OTA_JOB_TIMEOUT_S` 守卫。没有它，设备报了 done 却再没上线的任务会永远卡在
   `rebooting`——因为这个 elif 先匹配上，后面的超时判定分支永远够不着。

### 本仓库保留（比生产新，合并后一并生效）

- **20ms 定长分帧**——按"后加的为准"（09-01 > 08-31）。
- `_device_mqtt_ready()` + `"dispatching"` 占位（防 MQTT 发布与心跳下发的竞态）。
- OTA `accepted` 立即清 retained 命令。

### 一处有意的偏离：boot_announce 没有逐字照搬

生产版 `boot_announce` 用的是简单的 `mqtt_ok = _enqueue_mqtt_command(...)` 模式，
没有 `_device_mqtt_ready()` 预检、也没有 `"dispatching"` 占位。移植时**改用了本仓库
这套更防御的模式**，理由不是"顺手统一风格"，而是：

**今天刚修好的心跳回执 bug（见 `2026-09-14-heartbeat-fallback-empty-response-fix.md`）
让这个竞态从"不可达"变成"可达"。** 在此之前，心跳回执 body 恒为空，通过心跳下发的
命令设备根本收不到，所以"心跳抢先把 pending 标成 dispatched 并投递、设备执行并 ACK、
随后 MQTT 发布完成又把终态覆盖回 dispatched"这条竞态路径走不通。修复之后它走得通了，
所以照搬旧模式等于引入一个新可达的 bug。

### 新增

- `play_lan_audio` action（`POST /api/device/{id}/play_lan_audio`）：
  `params.name`（裸文件名，拒绝 `/` `\` `..`）→ 拼 `LOCAL_AUDIO_BASE_URL`；
  `settings_override.voice_volume_percent` → 现有 `volume` 参数；内部仍然下发固件
  认识的 `play_audio`，走 MQTT 而不是通用 `/command`（心跳轮询有 0~5s 排队延迟，
  实测数据见心跳那篇）。
- `LOCAL_AUDIO_BASE_URL` 环境变量（默认 `http://192.168.1.16:8080/esp32`），
  **必须与固件的 `CONFIG_LOCAL_AUDIO_BASE_URL` 保持一致**——固件的 URL 白名单只认
  这个前缀，单改一边会静默失效。

## 核对方式（不是"看着像对了"）

- 合并后逐行 diff 生产版本，把所有"生产有、合并版没有"的行单独列出来审了一遍，
  确认**每一条都是有意替换**（`pending`→`dispatching`、旧分包→20ms、旧 OTA 终态判断→
  带 accepted 清 retained 的版本），没有一条是漏掉的。
- 正面 grep 确认 `DISPATCHED_TIMEOUT_S` / `play: bool = True` / `_boot_announce_for` /
  `/api/boot_announce` / `play_lan_audio` / 20ms 分帧六项都在。
- `ota_manager.py` 合并后与生产**逐字节一致**。
- `static/` 四个文件与容器内**逐字节一致**。
- `python3 -m py_compile` 通过。

## 现状与遗留

- **代码合并完成并通过语法检查，但尚未部署、`play_lan_audio` 尚未在真机上验证。**
- 部署这件事本身还没有 CI：`.github/workflows/` 里只有 `build.yml` 和 `release-ota.yml`，
  **没有任何 workflow 部署 device_hub**。线上那份是 Codex 在 tang 上手改的
  （tang 上留着 `Dockerfile.bak-codex-20260913-ota-copy` 这个备份文件为证）。
  也就是说本仓库虽然是权威源码，但"从这里到生产"目前仍然是人工链路。
- 下一步：把合并后的 `server.py` + `ota_manager.py` 同步到 tang 并重建容器，然后用
  一条真实的 `play_lan_audio` 请求做端到端验证。
- 更长期的遗留：`wangyutang/device_hub/` 那份怎么处理？它现在仍然是"离线上最近"的
  一份（只差 OTA 那 17 行），但按项目方向它应该退役。建议在那边放一个指针文件说明
  权威源码已迁至 `esp32_wifi/cloud/device_hub/`，否则下一个 agent 打开 wangyutang
  仓库看到一份能跑的 device_hub，很容易又在错误的地方改起来——本次我自己就差点
  这么干。
