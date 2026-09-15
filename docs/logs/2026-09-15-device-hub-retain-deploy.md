# 2026-09-15 · 部署 retain=False 到生产，以及我造成的一次 502

## 做了什么

1. 清除 broker 上 60 个 retained command topic（其中 14 条未完成，含 **3 条 ota
   和 1 条 reboot**）
2. 生产 `server.py` 第 91 行 `retain=True` → `False`，重建容器
3. 命令往返从**分钟级**降到 **12–18 秒**

第 111 行的 `retain=True` **保持不变**——清除一条 retained 消息本身就要向同一
topic 发一条空载荷的 retained 消息，改掉它清除功能就废了。

## 我造成了一次 502，原因是漏抄容器参数

新容器起来后公网返回 **502**，而容器内 `curl localhost:8102/api/health` 是 **200**。

定位过程：

```
直连容器      200        → 应用没问题
docker port   正常       → 端口绑定没问题
→ 问题在 Caddy 侧
```

```
旧容器网络: wangyutang_platform_default
新容器网络: bridge          ← 我建容器时没加 --network
Caddy 网络: infra_default, wangyutang_platform_default
```

**Caddy 和新容器不在同一个网络上，解析不到 `device-hub`。**
`docker network connect wangyutang_platform_default device-hub` 后立即恢复 200。

### 这是同一个教训的第二次

`2026-09-14-device-hub-production-deploy.md` 里记着：

> **教训**：`docker inspect` 抓运行参数时，`.Config.*` 和 `.HostConfig.*` 都要抓全

那次漏的是 `--add-host`，这次漏的是 `--network`。而且我这次确实跑了 `docker inspect`
去抓参数——**抓了 Binds、PortBindings、ExtraHosts、RestartPolicy、Image、Env，
唯独没抓 NetworkSettings.Networks**。

上一篇的教训写成了"要抓全"，而"全"是什么并没有列出来。这次补上清单：

| 必抓项 | 命令 |
|---|---|
| 挂载 | `.HostConfig.Binds` |
| 端口 | `.HostConfig.PortBindings` |
| 额外 hosts | `.HostConfig.ExtraHosts` |
| 重启策略 | `.HostConfig.RestartPolicy.Name` |
| 环境变量 | `.Config.Env` |
| **网络** | **`.NetworkSettings.Networks`** ← 两次事故里漏的都在这类 |
| 镜像 | `.Config.Image` |

更稳妥的做法是不要手工重建：把这台机器的手工 `docker run` 改成 compose 或一个
脚本，让参数集合本身成为可复现的产物，而不是每次靠人从 inspect 里挑。
**这一项没做，记为待办。**

## 影响范围

- 公网 502 持续约 3 分钟（从重建容器到 `network connect`）
- 期间设备心跳走的是 `http://110.40.154.41` 明文兜底，**设备未掉线**
- 旧容器保留为 `device-hub-old-1789475603`，回滚路径可用

## 验证

```
health                200 ×3
设备 online           True，fw v77
ref_peak_dbfs         -84.3（静默，符合预期）
命令往返              15s / 18s / 12s
```

命令往返仍有十几秒，其中包含约 8 秒的心跳周期和我 4 秒的轮询粒度，**实际下发更快**。
与修复前的分钟级相比是数量级的改善。
