# OTA 自动化发布

## 设计

GitHub Actions 只负责构建并登记固件，不自动升级设备。操作人员在
`https://www.wangyutang.cn/devices/ota` 选择已发布版本和在线设备，确认后创建 OTA 任务。

完整状态链路：

```text
GitHub commit/tag
  -> Actions build
  -> SHA256/size validation
  -> POST /devices/api/ota/releases
  -> operator selects release + online devices
  -> POST /devices/api/ota/jobs
  -> MQTT accepted/progress/done
  -> device reboot
  -> heartbeat reports target firmware
  -> job verified
```

## 部署与恢复优先级

现场固定按以下顺序处理固件升级和故障恢复：

1. **OTA**：设备在线且 MQTT/心跳正常时，优先使用已登记的 OTA release 创建 OTA job，
   直到设备重新上线并将 job 标记为 `verified`。
2. **Spark USB**：OTA 无法使用、设备仍可通过物理 USB 连接时，优先在 Spark 上通过
   `/dev/ttyACM0` 使用 esptool/ESP-IDF 调试和烧录。Spark 是首选有线恢复主机。
3. **WSL USB**：仅当 Spark 不可用时，才使用 Windows `usbipd` 转发到 WSL 的
   `/dev/ttyACM0` 进行调试和烧录。

USB 恢复默认只写目标 app 分区，保留 NVS 中的 Wi-Fi、音量和设备身份；不得因为普通固件
升级而整片擦除。只有明确需要修复 bootloader、分区表或 NVS 损坏时，才扩大写入范围，
并在操作记录中说明原因和备份/恢复措施。

每次发布记录版本、Git commit、Actions URL、大小和 SHA256。每次 OTA 任务记录设备、旧版本、
目标版本、command_id、下载进度、离线/重启、重新上线、验证结果和错误信息。审计记录保存在
device_hub 数据卷中的 `ota_audit.jsonl`。

## GitHub Actions

工作流：`.github/workflows/release-ota.yml`。

触发方式：

- Git tag：`ota-<firmware-version>`
- GitHub Actions 页面手动运行，填写 `version` 和 `notes`

可选配置：

- Repository variable `OTA_PUBLISH_URL`：默认
  `https://www.wangyutang.cn/devices/api/ota/releases`
- Repository secret `OTA_PUBLISH_TOKEN`：云端设置同名环境变量后必须配置；未设置时当前联调阶段不鉴权。

Actions 构建的通用 OTA 镜像不包含 Wi-Fi 密码。设备从 NVS 的 `wifi_remote/ssid` 和
`wifi_remote/pass` 读取网络配置。本地凭据版首次启动时，如果该命名空间尚无凭据，会把
已验证的编译期主 Wi-Fi 写入其中；已有远程凭据不会被覆盖。完成这次 USB 引导后，后续
通用 OTA 镜像即可复用 NVS 凭据联网。

## 首次启用 rollback

Bootloader rollback 是 Bootloader 功能，不能仅通过应用 OTA 更新。现有设备需要一次 USB
刷写新版 `bootloader.bin`。首次本地凭据版固件使用编译期配置连接 Wi-Fi，并把配置持久化
到 NVS；完成这次引导后，后续版本可全部通过 GitHub Actions + OTA 页面发布。

## 成功标准

- OTA 命令收到 `accepted`
- 下载进度持续增长并到达固件大小
- 设备发送 `ota_applied` 后重启
- 新心跳的 `firmware` 等于目标版本
- `mqtt_connected=true`
- OTA 任务状态为 `verified`

如果新镜像在 120 秒内无法完成平台注册和 MQTT 健康检查，设备重启并由 Bootloader 回滚。

## 端到端验收记录（2026-09-01）

- Git tag：`ota-esp32-wangyutang-v6-ota`
- Git commit：`eae2db9d109fc08bfe5a7bd9634700d5562bf694`
- GitHub Actions：`https://github.com/1540530942/esp32_wifi/actions/runs/33420901758`
- 发布版本：`esp32-wangyutang-v6-ota`，release ID `rel-dbfc9f796386`
- 固件大小：1117168 bytes
- SHA256：`14882150745b8242d070eade1ffa209f3f21175c2b09a75da4d75a08247902b7`
- OTA job：`ota-a70a47f51921`，设备 `esp32-s3-walle`
- 任务创建：2026-09-01 01:45:44（Asia/Shanghai）
- 版本验证：2026-09-01 01:46:04（Asia/Shanghai），全链路约 20 秒
- 最终状态：`verified`；设备在线、`firmware=esp32-wangyutang-v6-ota`、`mqtt_connected=true`

持久化审计记录已验证包含 `release_published`、`job_created`、`command_published`、
多条 `download_progress`、`downloading`、`rebooting`、`verified` 和 `job_verified`。
这证明从 GitHub tag 构建、版本登记、网页选择设备下发、设备下载重启到新版本心跳确认的流程可全自动执行并回溯。

### 音频可靠性修复版本（2026-09-01）

- 版本：`esp32-wangyutang-v7-audio-fix`
- commit：`7c6ccd46aa7508597807390e6e6ef5aa1e28d9aa`
- Actions：`https://github.com/1540530942/esp32_wifi/actions/runs/33458132274`
- release：`rel-01fae9f76c67`
- OTA job：`ota-6cedbf48cf85`，最终状态 `verified`
- HTTP 回归：`c-ae4e8c`，MQTT 下发、8 KiB 流式 WAV 播放，状态 `done`
- PCM 回归：`c-02b0e8`，99,840 bytes，状态 `done`

该版本修复 HTTP 播报未发布 MQTT、整段 WAV 分配导致 `ESP_ERR_NO_MEM`、快速 ACK 被
`dispatched` 覆盖，以及 WebSocket/TLS 奇数字节分片造成 16-bit PCM 样本错位噪声的问题。

## API

```text
POST /devices/api/ota/releases
GET  /devices/api/ota/releases
GET  /devices/api/ota/firmware/{filename}
POST /devices/api/ota/jobs
GET  /devices/api/ota/jobs
GET  /devices/api/ota/jobs/{job_id}
GET  /devices/ota
```

创建 OTA 任务：

```bash
curl -X POST 'https://www.wangyutang.cn/devices/api/ota/jobs' \
  -H 'Content-Type: application/json' \
  -d '{"release_id":"rel-xxxx","device_ids":["esp32-s3-walle"],"force":false}'
```
