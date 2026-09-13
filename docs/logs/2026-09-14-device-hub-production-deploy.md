# 2026-09-14 · device_hub 分叉合并版部署到生产 + play_lan_audio 首次真机验证

承接 `2026-09-14-device-hub-fork-merge.md`。那篇只做到"代码合并完成、尚未部署"，
本篇记录把合并结果实际推上 tang 生产环境的过程和结果。

## 部署前备份

`tang:/root/wangyutang_platform/device_hub/` 是实际部署目录（不是 compose 管理，
是手工 `docker run`，`docker inspect` 的 Labels 是空的——这一点上一篇已经查过）。
覆盖前先备份，命名沿用 Codex 09-13 那次的习惯（`*.bak-codex-20260913-*`）：

```
server.py.bak-claude-20260914-fork-merge       (sha256 与覆盖前的 server.py 一致)
ota_manager.py.bak-claude-20260914-fork-merge  (sha256 与覆盖前的 ota_manager.py 一致)
```

两个 sha256 都在覆盖前用 `sha256sum` 核对过与原文件一致，不是"复制了但没验证"。

## 部署过程中的两个意外

### 1. Docker Hub 从 tang 访问不通

`docker build` 第一次直接失败：`dial tcp ...:443: i/o timeout`，拉不到
`python:3.12-slim`。查看本地镜像缓存发现之前的构建用的是国内镜像源
`docker.m.daocloud.io/library/python:3.12-slim`（已经缓存在本地）。加
`--build-arg PYTHON_IMAGE=docker.m.daocloud.io/library/python:3.12-slim` 后构建成功。
**这不是本次改动引入的问题，是这台机器一直以来的网络限制**，只是第一次手动构建
才会撞到（Dockerfile 里 `ARG PYTHON_IMAGE` 默认值是裸的 `python:3.12-slim`）。

### 2. 新容器起来后 MQTT 连不上——`--add-host` 漏加了

按 `docker inspect` 抓到的旧容器参数重建新容器，第一次没注意到旧容器还带着
`--add-host host.docker.internal:host-gateway`。Linux 上 Docker 不会像
Docker Desktop（Mac/Windows）那样原生解析 `host.docker.internal`，必须显式加这条
才能把这个域名指向宿主机网关。新容器起来后立刻用
`docker exec device-hub python3 -c "socket.gethostbyname('host.docker.internal')"`
验证，直接 `socket.gaierror`——**没有事后靠日志排查，是主动验证暴露的**。
补上 `--add-host` 参数重建后解析恢复正常（`172.17.0.1`）。

**教训**：`docker inspect` 抓运行参数时，`.Config.*` 和 `.HostConfig.*` 都要抓全，
`ExtraHosts` 藏在 `HostConfig` 里，光看 `Env`/`Cmd`/`Ports` 这些显眼字段会漏掉。

## 部署结果

- 旧容器 `docker stop` + `docker rename` 成 `device-hub-old-1789320760`（**没有删除**，
  是现成的回滚路径——真出问题 `docker rm -f device-hub && docker rename
  device-hub-old-1789320760 device-hub && docker start device-hub` 就能秒回）。
- 新容器 `device-hub`，镜像 `device-hub:local`（`28daaa6f...`），网络/端口/挂载卷/
  重启策略与旧容器一致（额外补了 `--add-host`）。
- 启动后立刻观察到真实设备流量（`esp32-s3-walle` 的心跳、前端页面的状态查询），
  容器本身健康。

## 验证

1. **基础健康**：`GET /api/device/esp32-s3-walle` 返回 `online: true`，
   `firmware: esp32-wangyutang-v36-heartbeat-fix`，`mqtt_connected: true`。
2. **心跳 body 没有被这次重建破坏**：重新 POST 一次 `/heartbeat`，仍然拿到完整
   JSON（`{"ok":true,"server_time":...,"commands":[]}`）——确认今天早些时候修的
   固件端 bug 和这次的服务端重建互不影响。
3. **`play_lan_audio` 首次真实调用**：
   ```bash
   curl -X POST .../devices/api/device/esp32-s3-walle/play_lan_audio \
     -d '{"params":{"name":"turn02_assistant.wav"},"settings_override":{"voice_volume_percent":40}}'
   → {"ok":true,"command_id":"c-e33c79","transport":"mqtt"}
   ```
   **第一次失败**：`status: failed`，`message: "stage=wav_start error=ESP_ERR_NO_MEM"`，
   从下发到失败仅 170ms（`dispatched_at: 0.0`，说明走的确实是 MQTT 立即投递，不是
   心跳轮询——接口设计本身没问题）。查当时设备状态：`largest_internal: 21504`
   字节——跟 `docs/aec/SUMMARY.md` 记录的已知基线（AFE 全双工常驻后
   `largest_internal ≈ 20480`，余量本来就很紧）完全吻合，**不是这次改动带来的
   新问题**，是这台设备本来就有的内存紧张状况在某个瞬间导致 WAV 播放缓冲区分配
   失败。

   **立即重试**，5 秒后拿到 `status: done`，`accepted_at → done_at` 约 8.16 秒
   （跟 turn02 实际时长 ~7.92s 吻合）。交叉核对 Spark 的静态服务器访问日志，
   `01:34:35` 那条 GET 与这次成功播放的时间点对应。**同一命令、同一设备状态类别，
   一次失败一次成功，确认是偶发的瞬时内存碎片问题，不是接口逻辑的必现 bug。**

## 现状

- **合并版 `server.py` / `ota_manager.py` 已经是生产实际运行的版本**，`server.py`
  的分叉问题至此解决。
- **`play_lan_audio` 已经过真机验证**，走 MQTT 下发（非心跳轮询），确认可用；
  但设备侧的 `ESP_ERR_NO_MEM` 偶发失败是一个真实存在、目前没有重试逻辑覆盖的
  边界情况——调用方（不管是这个接口还是别的 `play_audio` 调用方）都可能撞上，
  值得后续在 `play_lan_audio` 或更上层加一次自动重试，而不是要求调用方自己感知
  "偶尔失败很正常，请重试"。

## 遗留

- 老容器 `device-hub-old-1789320760` 暂时保留做回滚用，确认新容器稳定运行一段
  时间（比如覆盖至少一次 24h）后可以清理。
- `ESP_ERR_NO_MEM` 的自动重试没有做，只是这次验证顺带发现、记录下来。
