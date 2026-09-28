# korea SSH：用 TUN IP 排除取消固定网关依赖

## 用户诉求与连接方式

用户询问当前是否是双向建链，以及目标腾讯云服务器长期在线、公网 IP 固定，能否避免代理开关和本机换网带来的连接问题，并要求确认后实现。

`ssh -G korea` 确认当前目标是 `ubuntu@43.155.169.6:22`，没有 ProxyJump、ProxyCommand 或端口转发。SSH 由本机主动发起，返回数据使用同一连接；本次修复没有新增服务器反向隧道。

## 上一方案的问题

上一轮 Windows 持久路由将 `43.155.169.6/32` 固定经 `WLAN`、下一跳 `192.168.1.1` 出站。它恢复了当前网络的直连，但绑定网卡和网关，换网时可能失效。

## 当前实现

核对本机 Clash Verge 为 2.5.2，并检查对应版本源码的基础配置合并和 TUN 启用逻辑。基础 `config.yaml` 的 TUN 配置会进入生成的运行配置，切换 TUN 不需要指定物理网卡或网关。

在 Windows Clash Verge 基础配置中增加：

```yaml
tun:
  route-exclude-address:
  - 43.155.169.6/32
```

基础配置：`C:\Users\Administrator\AppData\Roaming\io.github.clash-verge-rev.clash-verge-rev\config.yaml`。修改前备份在同目录 `config.yaml.before-korea-exclusion-20260929.bak`。备份含代理配置，仅留本机，没有复制进仓库。

重启应用后，生成的 `clash-verge.yaml` 和核心命名管道 `/configs` 均确认包含排除项。通过核心本地控制接口暂时关闭 TUN，删除上一轮目标 IP 在 `PersistentStore` 和 `ActiveStore` 中的固定路由，再重新启用 TUN。当前 `PersistentStore` 中目标 `/32` 路由数量为零。

TUN 开启时，目标 IP 被排除出 TUN 接管范围，使用操作系统当前物理网络路由；TUN 关闭时使用普通系统路由。配置只固定服务器公网 IP，不固定本机网卡和网关。

## 实际验证

以下状态均用本机原有密钥执行 `ssh korea 'hostname; id -un'`，返回 `VM-0-5-ubuntu` 和 `ubuntu`：

1. TUN 关闭，上一轮固定网关路由已删除。
2. TUN 重新开启，运行配置包含目标 IP 排除项。
3. Clash Verge 应用及 mihomo 核心完全退出。
4. 应用重新启动，恢复原有 GLOBAL 和 TUN 开启状态。

核心控制接口最终确认 `mode=global`、`tun.enable=true`、排除项为 `43.155.169.6/32`。`Find-NetRoute` 显示当前自动选用 WLAN 的正常网关，且没有目标 IP 的持久静态路由。

完整退出测试的第一轮在检查退出状态时发生进程退出时序/输出编码问题，脚本的 finally 已重新启动代理；修正等待进程退出与输出解码后完整测试通过。测试辅助脚本位于 `/tmp/korea_proxy_lifecycle_check.py`，并非长期运行的守护程序。

## 边界与后续

没有实际切换到另一条 Wi-Fi/热点验证；已消除固定网关依赖，并验证代理开关和重启场景。换网中断现有 TCP 会话时仍需重新执行 SSH；当前网络自身必须允许直达目标 22 端口。服务器 IP 变更时需要更新排除项。无需服务器主动回连本机。

参考：Clash Verge v2.5.2 的 `src-tauri/src/enhance/mod.rs` 和 `enhance/tun.rs`；mihomo TUN 文档 https://wiki.metacubex.one/en/config/inbound/tun/ 。
