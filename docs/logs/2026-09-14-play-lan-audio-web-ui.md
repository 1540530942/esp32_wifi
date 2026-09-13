# 2026-09-14 · play_lan_audio 网页入口

用户确认 `play_lan_audio` API 已经可用后，要求补一个网页入口（此前只能 curl）。

## 做了什么

`cloud/device_hub/static/device.html` 的"远程播报"面板加第三个标签页
"局域网素材"，跟"文本播报""上传音频"并列：

- 5 个按钮对应 spark 上现有的 5 个文件（turn02/04/06/08/10_assistant.wav），
  按钮文案带了内容提示（开场白/桃花源记/饮酒其五/破阵子/望庐山瀑布），不是裸文件名。
- 复用页面已有的音量滑块（`selectedVolume`），映射到
  `settings_override.voice_volume_percent`。
- 复用已有的 `stop_audio` 下发逻辑。
- 请求体形状跟今天已经反复验证过的 API 完全一致
  （`{params:{name}, settings_override:{voice_volume_percent}}`），没有引入新的
  未验证路径。

## 部署

`static/` 是 Dockerfile 里 `COPY` 进镜像的，不是挂载卷，改单个静态文件不需要重建
整个镜像。先备份线上 `device.html`（sha256 核对一致），然后同时更新
`/root/wangyutang_platform/device_hub/static/device.html`（磁盘源，下次重建镜像
生效）和 `docker cp` 进运行中的容器（立即生效，因为 FastAPI 的 `StaticFiles`
每次请求都从磁盘读，不缓存在内存里）。两处哈希核对一致后确认部署完成。

## 验证的边界（如实说明）

- 确认了：页面 HTML/JS 语法正确（`node --check` 过），线上 `GET /devices/device`
  返回的内容里 5 个按钮、`play_lan_audio` 调用、"局域网素材"标签页文案全部存在。
- **没有做**：没有在真实浏览器里打开页面点按钮做视觉验证（这个环境没有浏览器/
  截图工具能访问局域网内的这个页面）。请求体的形状已经用同一套代码路径（curl）
  反复验证过，所以没有为了"验证网页"再触发一次真实出声——那只是重复验证已经
  确认工作的 API，不会带来新信息。如果按钮在浏览器里点击后有问题（比如 CSS
  布局、事件绑定），这次没有覆盖到，需要用户实际点一次确认。

## 追加（同日）：音量控件被藏起来了，用户报了这个问题

用户后续要求"增加音量的选项"。查了一遍才发现：`volume-control`（音量滑块）当时
写在 `#tab-text`（文本播报）内部，切到"上传音频"或刚加的"局域网素材"标签页时，
这个 div 跟着 `tab-body` 一起被 `hidden`——**看不见也调不了**，那两个标签页只能
静默沿用切换前在文本播报页面留下的音量值。这是加标签页时留下的真实缺口，不是
误解。

**修法**：把 `volume-control` 从 `#tab-text` 里挪出来，放到 `.tabs` 和三个
`tab-body` 之间，三个标签页共用同一份（`selectedVolume` 本来就是共享的 JS
变量，挪位置不用改脚本逻辑）。顺带加一行说明："上传音频"这条路径
（`upload_audio`）现在服务端根本不接收 volume 参数，所以这个滑块对它没有实际
效果——如实写清楚，不是承诺以后会修。

部署方式同上（`static/` 热更新，先备份哈希核对一致，`docker cp` 进运行中容器
立即生效，不用重建镜像）。`node --check` 过、div 标签配对数核对过（28/28）、
线上 `GET /devices/device` 确认新提示文案已经生效。同样没有做浏览器可视化验证。
