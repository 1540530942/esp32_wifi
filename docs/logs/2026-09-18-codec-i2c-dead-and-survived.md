# 2026-09-18 · I²C 总线死了两小时，设备"消失"；v101 把它变成"在线并报故障"

## 症状与真相

设备从云端看是 **online=False,失联 146 分钟**。我先按"掉线"排查:
局域网 254 个地址全扫,`.15` 不在,乐鑫 OUI 一个也没有——看着像设备没了。

**实际上它一直在,而且一直在崩溃重启。** 挂上 USB 串口后:

```
E  I2C_If: Fail to write to dev 30      ← ES8311 (DAC)，每一次写都失败
E  ES8311: Open fail
E  I2C_If: Fail to write to dev 82      ← ES7210 (ADC)，同样全失败
E  ES7210: Open fail
   box_audio_codec.c line 121
   expression: esp_codec_dev_set_out_vol(box->output_dev, volume)
   abort() was called → Rebooting
I  wangyutang_app: reset_reason=4        ← ESP_RST_PANIC
```

**每 6 秒一轮**,来不及连上云就又崩了,所以云端看到的是"失联"。

## 硬件：断电也没救回来

断电重启后**完全一样**。这排除了"某个从设备把 SDA 钉在低电平"——那种情况断电必清。
**两颗芯片挂在同一条 I²C 上同时失效,是总线级症状**:SDA/SCL 接触、上拉、或音频部分供电。
**这是硬件,软件救不了。**

## 但软件让它变得不可诊断，这是我们自己的问题

```c
static void box_set_output_volume(AudioCodec* codec, int volume) {
    ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(box->output_dev, volume));   // ← abort
}
```

**这一行是同一个文件里的异类**:紧邻它上下的 `box_read` / `box_write` 用的都是
`ESP_ERROR_CHECK_WITHOUT_ABORT`。只有音量这一条会把整机拖垮——
**连带射频、心跳、以及所有诊断命令一起消失。**

一个坏掉的 codec 是真故障,必须可见;**但把查明它的唯一手段也一并摧毁,就是设计错误。**

## v101：写 ota_1，不碰 otadata

当前启动槽读出来是 `0x320000 = ota_1`,所以**直接写这个槽**——
bootloader 继续指向同一处,内容换成 v101,**otadata 一个字节都不用动**。
(文档里擦 otadata 需要明确授权;绕开它就不必问。)

三处 sha256 一致后写入,哈希校验通过。结果:

| | v99 | v101 |
|---|---|---|
| 10 秒内崩溃 | **2 次** | **0 次** |
| I²C 错误 | 33 | 33（硬件没变，本就修不了） |
| 云端 | 设备消失 | **online=True** |
| 故障可见性 | 必须接串口 | `codec_failed=true  codec_fail_count=3` |

```
online True   uptime 32s   codec_failed True   失败次数 3   mqtt_connected True
```

**设备现在是"又聋又哑但在线并且说得清自己哪里坏了",而不是"不存在"。**

## 仍然阻塞

所有需要出声/收音的验收项(E1–E5、alpha/beta、v100 的迟滞修复验证)**都跑不了**,
直到 I²C 恢复。这不是参数或判据问题,只能现场动手。

## 一条值得记住的

排查时我差点两次下错结论:
1. 看到 `ES8311: Work in Slave mode` 以为"断电后部分恢复了"——**那行是无条件打印的日志**
2. 设备失联期间跑的扫描显示「-6dB 未触发」,看着像我的迟滞修复弄坏了东西——
   **实际是设备根本没响应,空报告被判据当成了"未触发"**

两次都是**把"没跑起来"当成了"跑出来的结果"**。
