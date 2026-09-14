# ESP32-S3 USB 读写与刷机规程

`WORKING.md` 规则 7 定的部署顺序是 **OTA → Spark USB → WSL USB**。本文只讲中间那一
环:**通过 spark 的 USB 对设备做串口读取和刷写**。

写这份文档的直接原因:2026-09-15 排查 TLS 分配失败时,OTA 整条链路不可用,只能走
USB。过程中在起串口、找 esptool、判断分区这几件事上反复试错。**下一次不要再试**,
按下面走。

---

## 0. 环境:不要安装任何东西

```bash
ESPTOOL=~/.espressif/python_env/idf5.5_py3.12_env/bin/esptool.py   # v4.12.0
PORT=/dev/ttyACM0
```

**为什么用这个而不是 pip 装一个:**

- spark 的系统 python 是 **PEP 668 保护**的(`/usr/lib/python3.12/EXTERNALLY-MANAGED`
  存在),`pip3 install esptool` 会被直接拒绝。用 `--break-system-packages` 是在共享
  机器上乱来。
- spark 的既有约定是 `~/venvs/` 下一个用途一个环境(`dl_env`/`hermes_env`/
  `modelscope`)。既然 IDF 环境里已经自带 esptool,再建一个就是纯重复。
- 它是 IDF **5.5** 的工具环境,而 CI 构建用 IDF **5.5.4**,同一代。

**权限**:`archer` 在 `dialout` 组,访问串口 **不需要 sudo**。

**spark 上没有 esp-idf 源码树**,只有工具环境 —— 所以**构建不能在 spark 做**,
只能刷。镜像来源见第 3 节。

---

## 1. 读:抓串口日志

设备自己的日志是排障最可靠的信息源。云端心跳会因为兜底逻辑显示"一切正常",
而串口不会骗人(2026-09-15 那次故障就是这么找到的)。

```bash
ssh spark 'cd ~/workspace/git_space/esp32_wifi && \
  timeout 115 python3 tools/serial_capture.py --port /dev/ttyACM0 --seconds 110'
```

### 三个踩过的坑

1. **端口独占**。串口同一时刻只能被一个进程打开。抓日志的进程没退干净,后面
   `esptool` 就连不上;反之亦然。开工前先看:

   ```bash
   ssh spark 'fuser -v /dev/ttyACM0'        # 谁占着
   ssh spark 'pkill -f serial_capture'      # 清掉抓取进程
   ```

2. **ssh 里用 `nohup ... &` 起后台抓取经常不落地**(进程随 ssh 会话消失,日志文件
   压根不生成)。**要前台跑**,用 `timeout` 限时;需要边抓边操作时,把**另一侧**
   (发命令的 curl)放到本地后台,而不是把抓取放后台:

   ```bash
   ( sleep 12; curl -X POST .../ota/jobs -d '...' ) &     # 本地后台触发
   ssh spark 'timeout 115 python3 tools/serial_capture.py --port /dev/ttyACM0 --seconds 110'
   ```

3. **AFE 会刷屏**。`Ringbuffer of AFE(FEED) is full` 和 `fetch: N results` 每秒几十行,
   会把关键错误淹掉。过滤掉再看:

   ```bash
   ... | grep -viE "Ringbuffer|fetch: [0-9]+ results" | grep -iE "ota|mqtt|tls|E \(|W \("
   ```

---

## 2. 读:探测芯片(非破坏,但会复位设备)

```bash
$ESPTOOL --port $PORT --chip esp32s3 --before default_reset --after hard_reset flash_id
```

正常输出应包含:

```
Chip is ESP32-S3 (QFN56) (revision v0.2)
Features: WiFi, BLE, Embedded PSRAM 8MB (AP_3v3)
Detected flash size: 16MB
MAC: 58:e6:c5:6b:9b:84
```

**这一步值得每次刷写前做一遍**:它同时验证了端口没被占用、复位控制(RTS)正常、
以及 flash 容量与分区表一致。省得写到一半才发现连不上。

---

## 3. 取镜像:走 CI,不在本地构建

**本机(ubuntu 工作站)构建不出完整镜像** —— 本地 IDF 是 5.3.3,而
`device_hub_client.cpp` 用了 `esp_http_client_config_t::addr_type`(新版才有),
必然在这个文件上编译失败。**这个报错与改动无关,不要试图靠升级本机 IDF 去"修"它。**

正确流程:

```bash
# 1) 改代码/配置 → 提交 → 打 tag(经 spark 中转推送,本地无 GitHub 凭据)
ssh spark 'cd ~/workspace/git_space/esp32_wifi && \
  git tag ota-esp32-wangyutang-vNN-xxx && git push origin ota-esp32-wangyutang-vNN-xxx'

# 2) 等 CI 构建并发布到 device hub,拿到文件名
curl -s 'https://www.wangyutang.cn/devices/api/ota/releases' | python3 -c "
import sys,json;r=json.load(sys.stdin);r=r.get('releases',r);print(r[0]['version'],r[0]['url'])"

# 3) 下载(HTTPS 走公网域名;设备侧用的是 http://110.40.154.41 那条,见下)
curl -o /tmp/vNN.bin 'https://www.wangyutang.cn/devices/api/ota/firmware/<filename>.bin'

# 4) 校验 SHA256 与 release 记录一致,再 scp 到 spark
sha256sum /tmp/vNN.bin
scp /tmp/vNN.bin spark:/tmp/
```

发布的 `esp32_wangyutang.bin` 是**纯 app 镜像**,不含 bootloader 和分区表 ——
正好对应"只写 app 分区"的安全要求。

---

## 4. 分区表(来自 `esp32_wifi_impl/partitions.csv`)

```
nvs       data nvs    0x9000    0x6000     ← WiFi 凭据等,默认绝不触碰
otadata   data ota    0xf000    0x2000     ← 记录哪个槽是活动的
phy_init  data phy    0x11000   0x1000
ota_0     app  ota_0  0x20000   0x300000   ← 3MB
ota_1     app  ota_1  0x320000  0x300000   ← 3MB
```

**没有 factory 分区。** 擦掉 `otadata` 后 bootloader 回落到 `ota_0`。

---

## 5. 写:刷入 app 镜像

```bash
ssh spark '
ESPTOOL=$HOME/.espressif/python_env/idf5.5_py3.12_env/bin/esptool.py
PORT=/dev/ttyACM0
pkill -f serial_capture 2>/dev/null; sleep 1

# 擦 otadata → bootloader 回落到 ota_0(NVS 不动)
$ESPTOOL --port $PORT --chip esp32s3 --before default_reset --after no_reset \
    erase_region 0xf000 0x2000

# 写 app 到 ota_0
$ESPTOOL --port $PORT --chip esp32s3 --before no_reset --after hard_reset \
    write_flash 0x20000 /tmp/vNN.bin
'
```

### 授权边界(照抄 `WORKING.md` 规则 7 并细化)

| 操作 | 是否需要明确授权 |
|---|---|
| 读串口、`flash_id`、`read_flash` | 否 |
| 写 `ota_0` / `ota_1`(app 分区) | 否 |
| 擦 `otadata` | **是** —— 严格说超出"只写活动 app 分区" |
| 擦 `nvs` | **是** —— 会丢 WiFi 凭据,设备将无法联网 |
| `erase_flash` 整片擦 / 重写 bootloader / 分区表 | **是** |

擦 otadata 的风险可控:USB 在手,写坏可以再刷。但**必须先问**。

### 为什么用 `--after no_reset` / `--before no_reset`

两条命令之间让芯片留在下载模式,避免中间复位一次跑起旧固件、再被打断。最后一条
用 `--after hard_reset` 让它正常启动。

---

## 6. 刷完验收

```bash
# 1) 串口:看新版本号和关键错误是否消失
ssh spark 'timeout 60 python3 ~/workspace/git_space/esp32_wifi/tools/serial_capture.py \
  --port /dev/ttyACM0 --seconds 55' | grep -iE "firmware|boot|mbedtls|fallback"

# 2) 云端:确认版本号变了
curl -s 'https://www.wangyutang.cn/devices/api/list' | python3 -c "
import sys,json
d=json.load(sys.stdin)
for x in (d.get('devices') or d):
    if x.get('device_id')=='esp32-s3-walle':
        s=x['state']; print(s.get('firmware'), 'largest_internal=', s.get('largest_internal'))"
```

**验收不能只看"版本号变了"。** 要确认这次刷写想解决的那个症状确实消失了 ——
例如 TLS 修复那次,判据是串口不再出现 `mbedtls_ssl_setup returned -0x7F00`、
心跳不再打 `recovered through direct HTTP fallback`。

---

## 7. 设备侧的两条 URL(容易混)

| 用途 | 地址 | 走 TLS? |
|---|---|---|
| 心跳/回执(首选) | `https://www.wangyutang.cn/devices/api` | 是 |
| 心跳/回执(兜底) | `http://110.40.154.41/devices/api` | 否 |
| OTA 固件下载 | 固件内把 `https://www.wangyutang.cn` 改写成 `http://110.40.154.41` | **否** |

**这个改写坑过一次**:看到 OTA 走 HTTP,就以为 TLS 故障和 OTA 无关。实际上 OTA 的
**下载**不走 TLS,但 OTA 的**命令送达**走 MQTT,而 MQTT 走 TLS。
详见 `docs/logs/2026-09-15-tls-alloc-failure-usb-path.md`。
