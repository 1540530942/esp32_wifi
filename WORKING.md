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

## 正在做的事（用完记得删）

（空 —— 目前没有人在做跨文件大改动）
