# 2026-09-14 · 心跳HTTP降级客户端提前半关闭导致响应体丢失（根因定位+修复）

## 背景

上一篇日志（`2026-09-13-spark-local-dialogue-playback.md`）记录过一个悬而未决的
现象：两条通过通用 `POST /api/device/{id}/command` 入口下发的 `play_audio` 测试命令
（`c-22f1e2`、`c-083e26`）停在 `dispatched`，设备没有访问 Spark，当时归因为
"MQTT下发链路的独立问题"。

本次会话是接着做 `play_lan_audio`（用户要求的接口重命名，语义等价于此前讨论的
"云端一条命令→ESP32从Spark局域网拉取播放"）之前，先把这个卡住的问题查清楚——
结果发现**根本不是MQTT的问题，MQTT命令从来没有牵扯进这条路径**，是设备心跳的
HTTP降级客户端有一个具体的、可稳定复现的bug。

## 复现

1. 现场重新发一条 `play_audio` 测试命令（`c-f74bee`），确认卡住是**当前正在发生**
   的现象，不是历史遗留：36秒轮询下来状态始终是 `dispatched`，Spark 侧
   `/tmp/esp32-local-audio-http.log` 完全没有设备来源的GET请求——**设备根本没有
   尝试去拉取文件**，说明命令内容从未真正送达设备执行，不是下载卡住。

2. 抓实时串口日志，逐行核对心跳流程：
   ```
   W device_hub: POST /heartbeat failed: ESP_ERR_HTTP_CONNECT
   W device_hub: public HTTPS unavailable; retrying http://110.40.154.41/devices/api/heartbeat
   I device_hub: raw connected to 110.40.154.41:80
   I device_hub: raw recv ended n=90 errno=119 total=90
   I device_hub: raw response head: HTTP/1.0 200 OK / Server: Caddy / Date: ... / Content-Length: 0
   I device_hub: heartbeat response bytes=0 body=
   ```
   **每一次心跳都是这个结果**——设备侧代码收到 `Content-Length: 0` 后立刻按协议
   正确终止读取（这段客户端代码本身没问题），但服务端真的返回了空 body。这意味着
   `process_commands()`（`device_hub_client.cpp`）第一行 `if (response.empty())
   return ESP_OK;` 每次都直接短路——**服务端往心跳回执里塞的任何 `commands` 数组，
   设备从来没有真正解析过**。

3. 用当前机器（跟设备不同网络）和 Spark（跟设备同网络/同ISP）分别对
   `110.40.154.41:80/devices/api/heartbeat` 发一模一样的 `HTTP/1.0` POST（先用41字节
   的极简body，再用597字节、跟设备真实 `state` 字段等大小的body），**两处都拿到了
   完整正确的响应**（`Content-Length: 50`，`{"ok":true,...,"commands":[]}"`，响应头里
   `Server: Caddy` + `Server: uvicorn` 两行都在，说明 Caddy 正常转发给了后端）。
   **排除了ISP链路劫持、包大小/TCP分片、服务端本身的问题。**

4. 怀疑固件这段"raw HTTP降级客户端"跟 curl 的差异点：代码在发送完整请求后、读取
   响应前，多做了一步 `shutdown(sock, SHUT_WR)`（半关闭，主动发 TCP FIN）。用
   Python 原始 socket **精确复现**固件的请求序列（发送 → `shutdown(SHUT_WR)` →
   读响应）：
   ```
   total bytes: 90
   HTTP/1.0 200 OK
   Server: Caddy
   Date: ...
   Content-Length: 0
   ```
   **跟设备实际收到的响应逐字节吻合**（只有 `Server: Caddy`，没有 `Server: uvicorn`，
   说明 Caddy 根本没转发给后端，是它自己合成的空响应）。去掉这一行 `shutdown` 调用、
   其余完全不变，重新发送：
   ```
   total bytes: 190
   HTTP/1.0 200 OK
   Content-Length: 50
   ...
   Server: Caddy
   Server: uvicorn

   {"ok":true,"server_time":...,"commands":[]}
   ```
   **响应立刻变完整。** 根因坐实。

## 根因

`esp32_wifi_impl/main/device_hub_client.cpp` 里 HTTPS 不可用时的降级 HTTP 客户端
（`post_http_raw`，被 `/heartbeat`、`/register`、`/log`、`/boot_announce`、`/ack`
共用），在发完请求、读响应前调用了 `shutdown(sock, SHUT_WR)`。这个调用的本意是
"我这边没有更多数据要发了"，属于常见的 HTTP/1.0 客户端写法，但**这台设备走的
Caddy 反向代理在探测到客户端主动半关闭（TCP FIN）时，会提前放弃这次代理转发，
直接合成一个空的 `200 OK`（`Content-Length: 0`）回给客户端，而不是等后端
（uvicorn/device_hub）处理完再把真实响应转发回来**。请求本身已经带了
`Content-Length` 和 `Connection: close`，服务端框定body边界完全不需要这个
半关闭信号。

## 影响范围

- **每一次**这台设备的心跳（`/heartbeat`）只要走了这条降级路径（而根据本会话及
  此前多次抓包的证据，HTTPS 在这条ISP路径上因为证书链问题**始终**不可用，也就是
  说心跳**始终**走这条路径），响应体就始终是空的。
- **`process_commands()` 因此从未真正执行过任何通过心跳回执下发的命令**——
  也就是任何走通用 `/api/device/{id}/command` 入口（不显式调用
  `_enqueue_mqtt_command()` 的那些）下发的命令，全部会静默失效，卡在
  `dispatched` 直到调用方自己的客户端超时。之前记录的 `c-22f1e2`/`c-083e26`
  就是这个bug的实例，不是巧合也不是设备重启导致的偶发。
- **不受影响的部分**：`/speak`、`/upload_audio`、`/test_audio` 这几个走
  `_enqueue_mqtt_command()`（MQTT主题订阅推送）的命令完全不经过这条降级HTTP
  客户端，一直工作正常——这也是为什么此前误判成"MQTT链路问题"：真正在正常工作
  的部分恰好都叫"MQTT"，而真正坏掉的部分（心跳HTTP降级客户端）跟MQTT毫无关系。
- `/register`、`/log`、`/boot_announce`、`/ack` 这几个调用方从不读取响应体内容
  （只关心请求是否发送成功），所以这个bug对它们没有可观察的影响，这也是它能
  存活这么久没被发现的原因——直到这次要用心跳通道下发命令才第一次真正依赖
  响应体内容。

## 修复

删掉那一行 `shutdown(sock, SHUT_WR);`，改成注释说明原因（防止以后有人为了
"更标准的HTTP/1.0写法"把它加回去）。改动位置：
`esp32_wifi_impl/main/device_hub_client.cpp` 的 `post_http_raw()`。

## 现状（截至本篇记录）

- **代码修复已完成，尚未编译、尚未OTA、尚未在真机上验证。**
- 修复思路已经用独立的 Python 脚本在真实网络环境（Spark 所在局域网）针对生产
  `110.40.154.41:80` 端点验证过，不是纸面推理。
- 下一步：编译（走 GitHub Actions，参考 v34/v35 的流程）→ OTA 发布 → 用一条新的
  `play_audio` 测试命令验证心跳下发命令这条路径现在能正常走通（设备真的去拉取
  Spark 文件、播放、并回 `ack`）→ 确认后再继续实现用户要求的 `play_lan_audio`
  这个新 action（`params.name` → 拼 `CONFIG_LOCAL_AUDIO_BASE_URL`，
  `settings_override.voice_volume_percent` → 现有 `volume` 参数）。
- 这个bug理论上也会让 `/register`、`/log`、`/boot_announce` 的响应处理逻辑
  （如果以后有人给它们加上读取响应体的逻辑）踩到同一个坑，值得在修复说明里
  留一笔，避免以后重新排查一遍同一件事。
