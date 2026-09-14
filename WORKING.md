# 协作须知（Claude / Codex 共用本仓库）

这个仓库同时被 Claude（Anthropic）和 Codex（OpenAI）两个 agent 用来开发 ESP32-S3
固件，没有分开维护副本——**共用同一份代码和 git 历史**，靠下面这几条规矩避免
互相覆盖对方的改动：

## 规矩

1. **开工前先同步**：`git fetch origin && git status`，如果远端领先就先
   `git pull --rebase origin <branch>` 合并进来再动手，不要在落后的基础上改。
2. **小步提交，改完就推**：一个功能点/一次修复对应一次提交，不要攒一堆改动到
   会话结束才一起提交——攒得越多，真发生冲突时冲突面越大、越难处理。
   提交后立刻 `git push`，不要让本地分支长时间领先远端。
3. **提交信息里带清楚身份**：commit message 结尾带上标识自己是哪个 agent
   （例如 `Co-Authored-By: Claude ...` / `Co-Authored-By: Codex ...`），
   方便 `git log` 追溯是谁改的。
4. **大改动前在这里知会一声**：如果要做涉及多个文件、牵一发动全身的重构
   （比如凭据表结构、命令处理的整体流程），先在下面"正在做的事"里写一行，
   完成/推送后删掉。不是强制锁，只是让对方看一眼就知道别碰同一块。
5. **`sdkconfig` 不进任何提交**——真实 WiFi 密码只在本地 gitignored 的
   `sdkconfig` 里，模板文件 `sdkconfig.defaults` 永远留空占位。
6. **会话记录写 `docs/logs/`，一个事件一个文件，不要追加进 `AEC_INTEGRATION_LOG.md`**：
   - 文件名 `docs/logs/YYYY-MM-DD-简短英文slug.md`（日期用当天，slug 描述这次做的事）。
   - **用中文写**，充分展开——不是记流水账，要让下一个打开仓库的 agent（不管是
     Claude 还是 Codex）不用重新问一遍就能接着做。至少覆盖：背景/用户原始诉求、
     做了什么排查或分析、发现了什么（尤其是代码里不直观的坑，比如隐藏的白名单、
     静默失败的分支）、生成或修改了什么产物及其存放位置、现状小结、留给下一个
     会话的决策点。
   - **纯分析、没有改代码的会话也要记**——不要求必须有代码 diff 才值得记录；
     分析结论、排查过程中发现的隐患、用户还没拍板的决策点，同样要留痕，避免
     下一个 agent 重新踩一遍已经踩过的坑或者重新问一遍已经问过的问题。
   - `AEC_INTEGRATION_LOG.md` 是历史遗留的单文件英文工程日志（L0-L6 验收那条线），
     只在续写那条主线时追加，不要把新话题也塞进去。
   - 参考格式见 `docs/logs/2026-09-13-multiturn-dialogue-audio-prep.md`。
7. **固件部署/恢复顺序固定为 OTA → Spark USB → WSL USB**：先尝试已登记且可验证的
   OTA；OTA 不可用时优先使用 Spark 上的 USB 调试/烧录链路；只有 Spark 不可用时才切换
   到 WSL 的 usbipd USB 链路。USB 恢复默认只写活动 app 分区并保留 NVS，整片擦除或
   重写 bootloader/分区表必须有明确授权。

## 正在做的事（用完记得删）

（空 —— 目前没有人在做跨文件大改动。`cloud/device_hub/server.py` 与生产的分叉已于
2026-09-14 合并并部署，详见 `docs/logs/2026-09-14-device-hub-fork-merge.md` 与
`docs/logs/2026-09-14-device-hub-production-deploy.md`。）

## ⚠️ 硬件阻塞：树莓派（turbopi-01 / raspberrypi）掉线

2026-09-14 07:10 之后失联，**三条独立路径全部不通**：

- 反向 SSH 隧道 `pi-reverse`（127.0.0.1:10024）→ Connection refused
- Tailscale `pi-tailnet`（100.118.92.117:22）→ Connection timed out
- `action_move` 服务端 `/api/diagnostics` → 无设备记录

远程无法恢复，**需要现场处理**（供电/网络/重启）。掉线前最后的活动是
`codex-loop-camera-check` 在 07:09–07:10 驱动机器人做前后左右移动——如果那个循环
还在跑，可能需要先确认它有没有把机器人开到没电或断网的位置。

**它挡住的是 AEC 打断的最后验收环节**：E2b（AEC 误伤检测）、E3（双讲）、E4（打断
延迟 <200ms）都需要树莓派扮演"房间里说话的人"。ESP32 侧能做的已经全部做完并通过
（P0、P1、E1、E2、E5、P3 实现），整体进度见
`docs/logs/2026-09-14-aec-bargein-status.md`。

树莓派一恢复就可以直接跑：

```bash
cd tools/aec_bench
python3 aec_bench.py --tag e2b --seconds 25 --settle 2 --no-play \
    --pi-play turn03_user.wav --pi-volume 100 --pi-delay 3
```
