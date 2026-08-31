# 设备 ↔ 平台 API 契约 (Device ⇄ Platform Contract)

> **这是设备侧（`1540530942/esp32_wifi`）与平台侧（`wangyutang.cn / device_hub`）之间唯一的耦合点。**
> 两个仓库各存一份完全相同的副本。任何字段/语义变更都必须先改这份文档、同步两边、再动代码。
>
> - **Contract version:** `v1`
> - **Base URL:** `https://www.wangyutang.cn/devices/api`
> - **Transport:** HTTPS，请求与响应体均为 `application/json; charset=utf-8`
> - **心跳周期:** 5s（控制延迟 ≤5s）
> - **鉴权:** v1 **暂不做**（从简）。所有接口开放，不校验令牌。预留位见 §1，规模化后再启用。
> - **兼容原则:** 平台对未知的多余字段必须忽略（向前兼容）；新增字段一律可选并带默认值；破坏性变更 → 升 `v2`，新增并行路径 `/devices/api/v2/...`，`v1` 保留过渡期。

---

## 0. 设计边界（为什么这样切）

- **设备侧只认这份契约**：固件不关心平台用什么框架、怎么存储、页面长什么样。
- **平台侧只认这份契约**：平台不关心设备是不是 ESP32、跑什么固件——只要按契约说话就接纳。用 `device_type` 字段区分设备种类，加新设备**不改平台代码**。
- **跨越边界的只有 HTTP + JSON**。除此之外两边内部完全独立演进、独立部署。

---

## 1. 鉴权（v1 暂不做）

**v1 从简，不做鉴权**：所有接口开放，请求无需携带任何令牌，平台不校验。
`Authorization` 头即便存在也被忽略。

**预留（不在 v1 实现，仅记录演进方向）**：将来规模化时，设备持 `Authorization: Bearer <DEVICE_TOKEN>`，
令牌烧录在固件本地配置（NVS/menuconfig，绝不入 GitHub），平台侧持对照表校验，失败返回
`401 {"ok":false,"error":"unauthorized"}`。启用它属于**破坏性变更前的可选加固**，不影响 v1 字段结构，
届时新增一个开关即可，无需升版本。

---

## 2. 数据模型

### 2.1 设备标识

| 字段 | 类型 | 说明 |
|------|------|------|
| `device_id` | string | 全局唯一，稳定不变。建议 `esp32-` + 芯片 MAC 后 6 位，如 `esp32-a1b2c3` |
| `device_type` | string | 设备种类，如 `esp32-s3`。平台用它分类，不做业务假设 |
| `name` | string? | 人类可读名字，可选，缺省用 `device_id` |

### 2.2 设备状态快照 (`state`)

心跳里上报的自由结构，平台**原样存储 + 展示**，不强制 schema。约定常用键：

| 键 | 类型 | 说明 |
|----|------|------|
| `firmware` | string | 固件版本，如 `xiaozhi-2.2.6` 或 `aec-fw-0.1.0` |
| `wifi_ssid` | string | 当前连接的 SSID |
| `wifi_rssi` | int | 信号强度 dBm，如 `-52` |
| `ip` | string | 局域网 IP |
| `uptime_s` | int | 本次开机运行秒数 |
| `free_heap` | int | 空闲堆字节（可选，诊断用） |
| `activated` | bool | XiaoZhi 激活状态（可选） |
| `volume` | int | 当前音量 0–100（可选） |

> 未来任何设备可自行扩展键；平台展示已知键，其余折叠进「原始数据」。

---

## 3. 接口（设备 → 平台）

### 3.1 注册 / 上线声明

```
POST /devices/api/register
```

请求：
```json
{
  "device_id": "esp32-a1b2c3",
  "device_type": "esp32-s3",
  "name": "客厅小智",
  "state": { "firmware": "aec-fw-0.1.0", "ip": "192.168.1.20", "wifi_ssid": "ChinaNet-dsge" }
}
```

响应 `200`：
```json
{
  "ok": true,
  "device_id": "esp32-a1b2c3",
  "server_time": 1756300000,
  "heartbeat_interval_s": 5,
  "config": {}
}
```

- 幂等：重复注册同一 `device_id` = 更新登记信息，不报错。
- `heartbeat_interval_s`：平台建议的心跳周期，设备**应当**遵循（默认 5）。
- `config`：平台可下发的设备侧配置（v1 留空对象，预留）。

### 3.2 心跳 / 状态上报

```
POST /devices/api/heartbeat
```

请求：
```json
{
  "device_id": "esp32-a1b2c3",
  "state": {
    "firmware": "aec-fw-0.1.0",
    "wifi_ssid": "ChinaNet-dsge",
    "wifi_rssi": -52,
    "ip": "192.168.1.20",
    "uptime_s": 3600,
    "free_heap": 142000,
    "volume": 60
  }
}
```

响应 `200`：
```json
{
  "ok": true,
  "server_time": 1756300030,
  "commands": [
    { "id": "c-77", "action": "reboot", "args": {} }
  ]
}
```

- **控制走心跳回执（长轮询友好，无需设备开服务端口）**：平台把待下发指令放进 `commands` 数组，设备在下次心跳时取回执行。因心跳 5s，控制延迟 ≤5s。
- 设备执行后通过 `POST /devices/api/ack`（见 3.3）回报结果。`commands` 为空数组时无事可做。
- 设备离线判定：平台侧超过 `3 × heartbeat_interval_s`（默认 15s）没收到心跳 → 标记 `offline`（容忍偶发丢包，避免抖动）。

### 3.3 指令执行回执

```
POST /devices/api/ack
```

请求：
```json
{
  "device_id": "esp32-a1b2c3",
  "command_id": "c-77",
  "status": "done",
  "message": "rebooting"
}
```
- `status`: `done` | `failed` | `unsupported`
- 响应 `200`：`{ "ok": true }`

### 3.4 运行日志上报（可选）

```
POST /devices/api/log
```
```json
{ "device_id": "esp32-a1b2c3", "level": "info", "message": "wifi reconnected", "ts": 1756300040 }
```
- `level`: `debug` | `info` | `warn` | `error`
- 平台环形缓冲保留最近 N 条（默认 200）。响应 `{ "ok": true }`。

---

## 4. 接口（平台 / 前端 → 平台）

供 `/devices/` 网页调用，**不需要设备令牌**（同源浏览器访问；写操作可加平台侧鉴权，见部署说明）。

### 4.1 列出设备
```
GET /devices/api/list
```
```json
{
  "ok": true,
  "devices": [
    {
      "device_id": "esp32-a1b2c3",
      "device_type": "esp32-s3",
      "name": "客厅小智",
      "online": true,
      "last_seen": 1756300030,
      "state": { "wifi_rssi": -52, "ip": "192.168.1.20", "firmware": "aec-fw-0.1.0" }
    }
  ]
}
```

### 4.2 设备详情
```
GET /devices/api/device/{device_id}
```
返回单台完整信息：标识 + `online` + `last_seen` + 完整 `state` + 最近日志 + 待处理指令。

### 4.3 下发控制指令
```
POST /devices/api/device/{device_id}/command
```
```json
{ "action": "reboot", "args": {} }
```
- 平台把指令入队，返回 `{ "ok": true, "command_id": "c-77" }`。
- 设备在下次心跳的 `commands` 里取到并执行。
- **v1 约定动作集**（设备可只实现子集，未实现的回 `unsupported`）：

| action | args | 语义 |
|--------|------|------|
| `reboot` | `{}` | 重启设备 |
| `set_volume` | `{ "value": 0-100 }` | 设置音量 |
| `identify` | `{}` | 闪灯/响一声，用于现场辨认 |
| `ota` | `{ "url": "..." }` | （预留）触发 OTA 升级 |

### 4.4 健康检查（对齐现有服务）
```
GET /devices/api/health  ->  { "status": "ok", "service": "device-hub" }
```

---

## 5. 错误约定

所有错误响应统一：
```json
{ "ok": false, "error": "<machine_code>", "message": "<human readable>" }
```
| HTTP | error | 场景 |
|------|-------|------|
| 400 | `bad_request` | JSON 格式/必填字段错误 |
| 404 | `not_found` | 未知 `device_id` |
| 429 | `rate_limited` | 上报过于频繁 |
| 500 | `internal` | 平台内部错误 |

> `401 unauthorized` 在 v1 不会出现（无鉴权），保留待将来启用令牌时使用。

---

## 6. 时序（典型一生）

```
设备开机
  └─ 连 WiFi
  └─ POST /register            → 拿到 heartbeat_interval_s (=5)
  └─ 循环：
       每 5s POST /heartbeat   → 若回执带 commands 则逐条执行
                               → 每条执行完 POST /ack
       事件发生 POST /log      （可选）
  └─ 断电/断网 → 平台 15s 后标记 offline
```

---

## 7. 变更记录

- **v1 (2026-08-27):** 初版。register / heartbeat(含指令回执) / ack / log；平台侧 list / device / command / health。控制走心跳回执模型（设备无需开端口），**心跳 5s，控制延迟 ≤5s，离线判定 15s**。**从简：v1 不做鉴权**（令牌为预留演进方向）。首批动作 `reboot`/`set_volume`/`identify`，`ota` 预留。
