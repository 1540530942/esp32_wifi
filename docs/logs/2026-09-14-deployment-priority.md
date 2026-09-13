# 2026-09-14 · 固件部署与恢复优先级

## 用户诉求

明确所有后续 agent 共同遵守的固件升级/恢复顺序：OTA → Spark USB → WSL USB。

## 写入位置

- `WORKING.md`：作为 GPT/Claude 每次协作开工必读的强制规则。
- `docs/OTA_RELEASE.md`：记录现场判定条件、顺序和 USB 写入安全边界。
- `docs/PROJECT_CONTEXT.md`：提供入口提示，指向 OTA 运维文档。

## 规则

1. 设备在线且 MQTT/心跳正常时优先 OTA，并等待 job `verified`。
2. OTA 不可用但 Spark USB 可用时，优先 Spark 的 `/dev/ttyACM0`。
3. 仅 Spark 不可用时才切换 WSL 的 usbipd USB 链路。
4. USB 恢复默认只写 app 分区并保留 NVS；重写 bootloader、分区表或擦除 NVS 必须有明确授权。

## 当前证据

2026-09-13 已通过 GitHub tag 触发 Actions 并完成 v35 OTA，job `ota-c6de382c49e1`
最终为 `verified`。此前 OTA 失败时使用 Spark USB 恢复，未擦除 NVS；WSL 作为备用链路保留。
