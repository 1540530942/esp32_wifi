# 2026-09-27 · 当前进展梳理与 Spark USB 连接检查

## 背景与用户诉求

用户先要求确认仓库当前与远端的差异、同步最新代码并梳理进展，随后询问是否能
连接 ESP32。目标是弄清楚当前 AEC 工作处于什么状态，以及现场是否仍有一条可用的
调试/恢复链路；本次**没有改固件、没有发布 OTA、没有向设备下发命令**。

## 代码同步与当前基线

开始时本地 `feature/aec-fullduplex` 比远端落后 149 个提交，工作区没有未提交修改。
按协作规则执行 `git fetch origin` 后，以 rebase 方式同步；由于本地没有私有提交，
实际是快进合并。

同步后的 HEAD 为：

```text
bb84607 docs: alpha/beta re-verified on v104, P0-P3 task list complete
```

同步后 `git status --short --branch` 显示本地与
`origin/feature/aec-fullduplex` 一致且工作区干净。

这批更新包括 AEC/打断实现和台架工具、设备控制面改动、部署/恢复文档，以及
2026-09-13 至 2026-09-19 的验证记录。当前行为应以源码和这些近期记录为准，
不能再以同步前的旧临时工作目录为准。

## 最新功能进展（以 v104 后复测记录为准）

阅读 `docs/logs/2026-09-19-e5-recheck-and-p2-ladder-complete.md` 与
`docs/logs/2026-09-19-ab-recheck-post-recovery.md` 后，当前阶段结论如下：

- P0：前置诊断和异常存活处理完成；codec I2C 异常时设备不再整体退出。
- P1：MIC/REF 两路 PGA 独立配置、开机顺序和回采增益校准已完成。
- P2：E1 至 E5 在硬件恢复后按顺序复测并通过。E4 有效样本的打断延迟为
  118–161 ms，均小于 200 ms；E5 在 127.52 秒连续纯播报中为 0 次误打断。
- P3：本地 VAD 判定、立即停播和扬声器播放位置上报已接入；alpha/beta 在 v104
  上复测 4/4 通过。其中 beta 的理想双通道样本表明原始麦克风包含机器人播报，
  AFE 输出正确保留了人声。
- P4（Spark 流式 ASR/LLM/TTS）仍在本阶段范围外。它曾挤占内部 RAM 并导致 HTTPS
  心跳回落；现在由 `CONFIG_AEC_STREAM_TO_CLOUD` 控制且默认关闭，不能在未重新做
  内存预算和验证时打开。

因此，本阶段目标「机器人说话时用户开口，200 ms 内停播」有最新端到端复测证据支持。

## ESP32 连接排查

### 当前主机与直连尝试

当前开发环境没有 `/dev/ttyACM*`、`/dev/ttyUSB*`，所以未发现可直连的 ESP32
串口。直接 SSH 名称 `spark` 在当前环境也无法解析；这只说明当前环境未配置该
主机的解析/跳板路径，并不能说明 Spark 或 ESP32 自身不可用。

### 经 korea 跳板检查 Spark

按用户指定路径执行 `korea -> spark` 的只读检查，成功进入主机：

```text
spark-c9a7
```

在 Spark 上检查以下项目：

- `/dev/ttyACM*`、`/dev/ttyUSB*` 与 `/dev/serial/by-id/*`；
- USB 设备枚举（`lsusb`）；
- 串口占用查询。

结果：没有任何上述串口节点；USB 枚举只包含 root hub、Realtek USB Hub 和无线网卡，
没有 ESP32-S3 原生 USB-Serial/JTAG 应出现的 `303a:1001`。因此这不是串口被某个
进程独占的问题，而是**设备目前没有被 Spark 的 Linux 系统枚举到**。

第一次经跳板检查已得到 `/dev/ttyACM0 does not exist`；之后一次受沙箱网络限制的
重试失败，使用同一只读命令在授权网络环境重试后得到上述完整 USB 枚举，结论一致。

## 现状小结与后续决策点

1. 代码已经是远端最新的 `bb84607`，无需再次 pull；后续动手前仍须按
   `WORKING.md` 执行 fetch/status。
2. 软件验证的 P0–P3 已有近期复测证据；但长期稳定性与 OTA 回滚安全网的历史待办
   仍需在开始发布工作前重新核验，不能因本次进展梳理而默认关闭。
3. 当前无法通过 Spark USB 调试或刷写。下一步需要现场确认 ESP32 的供电、USB 数据线
   和物理连接/透传归属，直至 Spark 上能重新出现 `303a:1001` 和对应
   `/dev/ttyACM0`（或其他 `/dev/ttyACM*`）节点。
4. 在 USB 恢复之前，如需操作在线设备，应先选择只读云端心跳/API 核验；任何 OTA
   或 USB 写入仍必须遵循 `docs/OTA_RELEASE.md` 与 `docs/USB_RECOVERY.md` 的优先级和
   安全边界。

