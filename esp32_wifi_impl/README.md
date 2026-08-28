# ESP32 XiaoZhi / wangyutang.cn 设备客户端

这是 `bread-compact-wifi-lcd`（ESP32-S3）设备侧的最小 ESP-IDF 客户端，负责：

- 连接 2.4 GHz Wi-Fi；
- 向 `device_hub` 注册设备；
- 每 5 秒上报心跳和设备状态；
- 接收网站下发的命令并回传 ACK；
- 为现有 XiaoZhi 的音频播报、麦克风和屏幕能力预留集成边界。

当前版本不替换 XiaoZhi 音频引擎，也不包含任何真实 Wi-Fi 密码或设备令牌。它应作为 XiaoZhi 工程中的设备平台层合并，或者作为联调基线使用。

## 构建

```bash
idf.py set-target esp32s3
idf.py menuconfig
idf.py build
idf.py -p COM4 flash monitor
```

在 menuconfig 的 `Wangyutang device hub` 中配置：

- Wi-Fi SSID / password；
- `https://www.wangyutang.cn/devices/api`；
- 唯一设备 ID。

当前 v1 不使用令牌鉴权。不要把 `sdkconfig` 或 Wi-Fi 密码提交到仓库。

## 音频集成

当前 XiaoZhi 固件已具备音频播报和麦克风聆听能力。该客户端只负责联网和设备管理；后续应将 `DeviceHubClient` 合并到 XiaoZhi 的 `bread-compact-wifi-lcd` 构建中，并由现有 AudioCodec 继续处理：

```text
麦克风 GPIO: WS=41, SCK=39, DIN=47
扬声器 GPIO: DOUT=48, BCLK=2, LRCK=1
```

## 当前鉴权约定

- 当前 v1 使用 HTTPS，但暂不使用设备令牌鉴权；
- 平台不应直接开放 ESP32 服务端口；
- 危险命令暂由平台接口控制；后续如需公网安全控制，再设计 v2 令牌鉴权。
