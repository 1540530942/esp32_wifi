# 2026-09-15 · TLS 分配失败:设备失去 HTTPS/MQTT/WS,OTA 随之不可用

## 症状链

v55 固件连续三次 OTA 失败,都是 `OTA verification timeout`,设备始终不重启、无回执。

串口(spark `/dev/ttyACM0`)给出真相:

```
E esp-tls-mbedtls: mbedtls_ssl_setup returned -0x7F00
E esp-tls: create_ssl_handle failed
W device_hub: public HTTPS unavailable; retrying http://110.40.154.41/...
I device_hub: POST /heartbeat recovered through direct HTTP fallback
E websocket_client: Could not lock ws-client within 50 timeout
W ws: disconnected
W ws: uplink gated: ready=0 connected=0 sent=20324 dropped=28049
I device_hub: heartbeat response bytes=50 body={"ok":true,...,"commands":[]}
```

`-0x7F00` = `MBEDTLS_ERR_SSL_ALLOC_FAILED`。**所有基于 TLS 的连接都死了**:
HTTPS、WebSocket、以及 **MQTT**。

## 为什么是分配失败

```
CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN=16384          TLS 输入缓冲要 16KB 连续内存
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384        ≤16384 的分配一律走内部 RAM
CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC is not set     mbedTLS 不使用 PSRAM
```

设备实测 `largest_internal = 7936`(总空闲 26KB,但碎成了小块)。mbedTLS 申请的
**恰好是 16384 —— 不多不少卡在阈值内侧**,被强制留在内部 RAM,然后失败。8MB PSRAM
全程用不上。

碎片化是**分钟级**的,不是长期累积:重启后 `largest` 13312,**10 分钟后就掉回 7936**。
所以"重启一下再 OTA"救不了——接受命令那一刻内存最好,依然失败。

## OTA 为什么被连累(绕了一层)

OTA 的**下载**不走 TLS —— 固件里有改写逻辑:

```cpp
constexpr const char* kOtaHttpsHost = "https://www.wangyutang.cn";
if (ota_url.rfind(kOtaHttpsHost, 0) == 0)
    ota_url = std::string("http://110.40.154.41") + ota_url.substr(...);
```

实测这个 HTTP 地址完全正常(200,2453936 字节,SHA256 吻合)。

但 OTA 的**命令送达**走 MQTT,而 MQTT 走 TLS。链条是:

```
命令发布到 MQTT → 服务端标记 dispatched → 设备收不到
→ 心跳不再补发(commands:[]) → 600s 超时 → failed
```

服务端 `pending_commands` 里积压 55 条,全卡在 `dispatched`。

**排查中我走错过两次,都记在这里免得重演**:

1. 先怀疑"磁盘满导致固件文件损坏"——下载实测 SHA256 逐位吻合,**假设错误**
2. 看到 OTA 改写成 HTTP,就断言"TLS 解释不了 OTA 失败"——**这个纠正本身是错的**,
   TLS 确实解释得了,只是经由 MQTT 命令通道而不是下载通道

教训同前几轮一致:**沿着一条路径证伪,不等于整个结论被证伪**。

## 为什么一直没人发现

因为心跳有**明文 HTTP 兜底**(本会话早先为修另一个问题加的)。每次心跳都在降级,
但云端看到的是"设备在线、状态正常",`mqtt_connected` 甚至仍报 `true`。

**这个兜底把一个严重故障变成了完全静默的降级。** 所有仪表盘是绿的。

## 修法与路径

改动**只有一行**(一次只动一个变量):

```
CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y
```

让 mbedTLS 显式从 PSRAM 分配,绕开 `SPIRAM_MALLOC_ALWAYSINTERNAL` 那条策略。

**必须走 USB,因为无法用 OTA 修 OTA。**

### USB 环境:不需要安装任何东西

spark 上已有 IDF 5.5 的工具环境,里面自带 esptool:

```
~/.espressif/python_env/idf5.5_py3.12_env/bin/esptool.py   → v4.12.0
```

选它的理由:spark 的系统 python 是 PEP 668 保护的(`pip3 install` 会被拒);
不新建 venv、不动 `~/venvs/` 下已有的 dl_env/hermes_env/modelscope;
而且它是 IDF **5.5** 的环境,与 CI 的 5.5.4 同代。`archer` 在 `dialout` 组,无需 sudo。

spark 上**没有** esp-idf 源码树,只有工具环境 —— 所以构建仍走 CI,spark 只负责刷。

### 通路已验证

```
Chip is ESP32-S3 (QFN56) (revision v0.2)
Features: WiFi, BLE, Embedded PSRAM 8MB (AP_3v3)     ← 修复前提成立
Detected flash size: 16MB                             ← 与 partitions.csv 一致
MAC: 58:e6:c5:6b:9b:84
Hard resetting via RTS pin...                         ← 复位控制正常
```

### 刷写方案(待执行)

分区是双 OTA、无 factory:

```
nvs      0x9000   0x6000     ← 不碰,保留 WiFi 凭据
otadata  0xf000   0x2000     ← 擦除,让 bootloader 回落到 ota_0
ota_0    0x20000  0x300000   ← 写入新镜像
ota_1    0x320000 0x300000
```

**灰区**:`WORKING.md` 写的是"USB 恢复默认只写活动 app 分区并保留 NVS"。擦 otadata
不属于 NVS/bootloader/分区表,但严格说超出了"只写 app 分区",需要明确授权。
风险可控——USB 在手,写坏可以再刷。

## 验收判据

刷入后看串口:`mbedtls_ssl_setup` 报错消失、心跳不再打
"recovered through direct HTTP fallback"、`ws` 不再反复断开。然后 OTA 恢复可用,
v55 的 T3.2 修复可以走正常流程下发。
