# 2026-09-15 · 下发延迟的真正成因：通用命令端点根本没发 MQTT

## 结论先说

| | 修前 | 修后 |
|---|---|---|
| 服务端实测（`done_at - created_at`） | — | **169ms**（最大 176ms） |
| 探针墙钟中位 | **10.9s**（最大 49.1s） | 3.5–4.1s（含探针自身开销） |
| transport | `None`（走心跳兜底） | **`mqtt`** |

**10.9s → 169ms，64 倍。** 并且已经稳稳落在 P4 的 <1s 预算里。

## 找法

我一直把"命令下发慢"当成一个平台级的模糊问题，并且先后归因给
retained 重放和磁盘写满。**那两个都真实存在、也都确实修了，但都不是这一个。**

磁盘修完后重测，中位仍是 10.9s，说明和磁盘无关。10.9s ≈ **两个 5s 心跳**，
这个数字形状很有指向性：设备心跳 5s 一次（MQTT 连上时），一次心跳把命令取走、
再一次心跳把回执带回来。

于是去看设备怎么收命令——`device_hub_client.cpp:287`，命令是**从心跳响应里取的**。
MQTT 是应该有的快路径。查 `mqtt_connected=True`、`mqtt_disconnect_count=0`，
链路好好的；主题两边也对得上（设备订阅 `devices/<id>/command/#`，
服务端发 `devices/<id>/command/<cid>`）。

那就只剩一种可能：**服务端压根没发**。

`server.py` 的通用端点 `POST /api/device/{id}/command` 只是把命令塞进队列标
`pending` 就返回了，从头到尾没调用 `_mqtt_publish_command`。
而 `play_audio` 那条路径（第 596 行）是发的。

**只有 play_audio 走 MQTT，其它全部靠心跳轮询兜底。**

## 为什么这么久没被发现

因为**兜底一直在工作**。心跳最终总会把命令送到，功能上没有任何东西是坏的，
只是慢。一个"能用但慢"的路径不会报错、不会进日志、不会有人去查，
它只会让人以为"这个平台下发就是慢"。

这和本次工作里反复出现的是同一类问题：**一个健在的兜底掩盖了一条死掉的主路径**。
前面是 HTTPS 失败回落明文、AFE 配置被 `afe_config_check()` 静默改写，这是第三次。

## 影响不止于测试工具

**云侧发起的 `stop_audio` 走的就是这条路**，此前要等约 11s。

设备端打断不受影响——它完全不出 ESP32，实测 152ms，
这一点没有变。但任何从云端发起的停播都白白背上了整个轮询延迟。

## 改动

通用端点照抄 `play_audio` 已有的模式：先占位为 `dispatching`，再 publish，
然后按结果落 `dispatched`/`pending` 和 `transport`。

**没有重复执行的风险**：心跳只取 `status == "pending"` 的命令
（`server.py:442`），MQTT 发成功的会变成 `dispatched`，心跳不会再发一遍；
发失败的落回 `pending`，心跳照旧兜底。这正是 `play_audio` 一直在用的结构。

冒烟验证：`set_volume` 两次往返，每个 id 各一条记录、`status=done`、
`transport=mqtt`，音量确实落地；`turn02` 播放 `spk_played_ms=7920`
（应为 7920），无截断、无误触发打断。

## 顺带：部署脚本化了

`cloud/device_hub/deploy.sh`。这条一直记在待办里没做，而它已经造成过两次事故：
漏 `--add-host` 连不上 broker，漏 `--network wangyutang_platform_default`
导致全站 502。**两次我都跑过 `docker inspect`，只是没把那个字段读出来。**
写在文档里的检查清单挡不住"凭记忆重敲一遍命令"，所以清单改成脚本本身。

两个坑记下来：

1. device_hub **不在** `/root/wangyutang_platform/docker-compose.yml` 里。
   那个 compose 只负责创建 `wangyutang_platform_default` 网络，容器是手工起来
   再连上去的——**这正是重新部署时容易漏掉网络的原因**。
   把它并进 compose 是更好的修法，但那是十几个服务共用的文件，记为待办，没顺手做。
2. tang **连不上 registry-1.docker.io**（build 报 dial timeout，`docker run alpine`
   也一样）。Dockerfile 的 `ARG PYTHON_IMAGE` 就是为此准备的，指向本地已有的
   `docker.m.daocloud.io/library/python:3.12-slim` 即可，重建不需要外网。

脚本带 `--check`，只校验不改动：网络、`host.docker.internal`、健康检查。
构建失败时 `set -e` 会在动容器之前停住——这次真的发生了，线上没受影响。
