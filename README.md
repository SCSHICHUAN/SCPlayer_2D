# SCPlayer_2D

基于 FFmpeg + OpenGL ES 的 iOS 音视频播放器实验工程（以音频为主时钟做 A/V 同步）。

本文记录实现过程中踩过的坑，方便后续接入或重构时避开。

---

## 1. 音视频同步的坑

同步核心是：**视频跟音频走**（`AV_SYNC_AUDIO_MASTER`）。

```text
diff = 视频帧 pts − 音频时钟
```

### 最大的坑：解码等渲染完成（解耦失败）

早期最容易写成：

```text
解码一帧 → 交给渲染 →【卡住】等 GL / 回调完成 → 才继续解码下一帧
```

这等于把 **解码节奏绑在渲染耗时上**：

| 后果 | 说明 |
|------|------|
| 解码与渲染耦合 | 渲染一慢，解码立刻饿死；`pictq` 填不满，同步无帧可调 |
| 主时钟对不齐 | 刷新线程在等 GL，`delay` / `diff` 再准也没用，实际显示被渲染拖着走 |
| 队列语义混乱 | 「显示完再 pop / 再通知解码」会让出队、busy、同步缠在一起，难查、易卡死 |
| 模拟器更惨 | GLES 全屏填充贵时，整条管道被 render 限速，表现为卡顿、音画各走各的 |

**正确模型：两边各自跑，用帧队列缓冲衔接。**

```text
解码线程：解码完成 → 拷贝/入队到 pictq → 马上解下一帧（把缓存帧尽量填满）
刷新线程：按 pts vs 音频算 delay → 到点 clone 送显并立刻出队 → 不等人家画完
渲染侧：  只画手里的拷贝，画完 av_frame_free + 清 display_busy
```

要点：

1. **解码不要等渲染**：入队后立刻继续解，目标是冲满 `pictq`，不是「显示一张再解一张」
2. **送显用拷贝**：`av_frame_clone` 后立刻 `pop`，所有权交给 UI；模块侧不再握那帧
3. **`display_busy` 只挡送 GL**：忙则本帧可丢显/不出屏，但**不要**用它卡住解码，也**不要**把它算进 `delay`
4. **同步只认时钟**：刷新节奏由 `pts − 音频时钟` 决定，与 GL 完成时刻无关

当前做法：`video_display` 里 clone 后立刻出队；`display_busy` 时出队丢显，不参与同步计算。

### 非异常时，`diff > 0`（视频超前）

直觉上：**再等 `diff` 就可以对齐音频再播**。

本工程实测更跟手的路径是直接：

```text
delay ≈ max(0, diff)   →  先送显，再按 delay 休眠
```

（见 §7「音视频同步测试」。）源码里另有对齐 ffplay 的「改帧间隔做平滑」写法，过渡更柔但贴音频会略钝，未作为默认。

若做平滑（ffplay 思路），是在帧间隔 `delay` 上修正，而不是裸等整个 `diff`：

| 情况 | 做法 | 用意 |
|------|------|------|
| `diff >= sync_threshold` 且 `delay > framedup(≈100ms)` | `delay = delay + diff` | 长帧：把超前量补进等待 |
| `diff >= sync_threshold` 且短帧 | `delay = 2 * delay` | 不一次吃满 `diff`，先拉长一帧显示，慢慢对齐 |
| `\|diff\|` 小于阈值 | 不改 `delay` | 小误差当噪声，避免抖 |

### `diff < 0`（视频落后）

- 当前 tmp：`delay = 0`（不睡），尽快刷，用后续帧追音频
- ffplay 风：`delay = max(0, delay + diff)`，缩短等待慢慢追

`diff < 0` 时不要把负数丢进无符号 sleep（会变成超长休眠）。`sc_delay_ms` 仅在 `ms > 0` 时 `usleep`。

### 其它常见误区

| 误区 | 后果 |
|------|------|
| **等渲染完成再回调/再解码**（最大坑） | 解码被 GL 限速，缓存填不满，同步名存实亡 |
| 用墙钟硬睡「固定帧间隔」当同步 | 音频一抖视频就漂 |
| 把 `display_busy` / GL 完成状态掺进同步 `delay` | 渲染一慢，整条刷新被拖死 |
| `actual_delay < 某最小值` 就丢帧不显示 | 到点该画的帧反而被扔掉 |
| 启动阶段音频时钟还是 `NAN` 就狂追 | 开头乱丢帧 / 乱加速 |
| 音频暂停后仍用冻结的 audio clock 算 `delay=pts−audio` | `diff` 越来越大，视频越睡越久，看起来「声停画也停」 |
| 单位混用秒 / 毫秒，或负 delay 转成 `uint` | sleep 天文数字或完全对不齐 |

原则：**解码冲缓存、刷新认时钟、渲染只消费拷贝；三件事不要串成一条同步等待链。**

---

## 2. 渲染跟不上

模拟器上全屏 YUV 片元填充很贵；真机通常还能跟上，但一旦 GL 比帧间隔慢，会出现：

- 主线程上积压多帧回调
- 或忙时持续丢显示帧（画面顿、跳）

对策：

1. **解耦**：解码 / 刷新队列不要等 GL 完成再 `pop`（clone 送显，回调里释放拷贝）
2. **门闩**：`VideoState.display_busy` 由模块置位，接入方显示完清 0；忙则本帧不送 GL，但同步照走出队
3. **测真实耗时**：GLES 异步提交，不用 `glFenceSync` / `glFinish` 时，`draw≈0` 是假象

不要用「等渲染完再算下一帧 delay」来假装同步——那是在用渲染速度限制数据模块。

---

## 3. 没有对象化，全局太多

早期容易写成：

- 全局 `VideoState *`
- 全局回调 / 全局 Render 指针
- C 与 OC 互相直接摸静态变量

问题：生命周期乱、无法多实例、难测、一崩溃全挂。

方向：

- 状态收进 `VideoState`，通过 `opaque` / `userData` 下传
- 播放入口：`scplayer(path, fn_call, userData)`
- UI 只持有自己的 `SCRender` / AudioQueue，完成后写回 `is->display_busy = 0`

模块约定要写进回调契约里，否则换个接入方就忘清 `display_busy`、忘 `av_frame_free`。

---

## 4. GL 的 `contentsScale` 导致纹理 / FBO 过大

`CAEAGLLayer.contentsScale`（以及 `contentScaleFactor`）会直接放大 renderbuffer：

```
像素宽 ≈ bounds.width × contentsScale
```

模拟器上 scale=2 时 FBO 可到约 `828×1472`，全屏 YUV draw 可到几十毫秒；误设成更大（甚至错误的超大 scale）会导致：

- FBO 分配失败 / `Frame buffer is not completed`
- 填充率暴涨，渲染更跟不上

经验：

- 模拟器可先用 `contentsScale = 1` 换流畅，再谈画质
- 真机 Retina 再按需提高 scale
- `contentScaleFactor` 与 `contentsScale` 保持一致
- 改 scale 后必须按新尺寸重建 color/depth renderbuffer

---

## 5. 音频时钟不准（深刻问题）与取舍

音频主时钟要回答：**喇叭现在播到时间轴的哪一点**。下面分三层：问题本质 → 本工程方案 → 终极方案。

### 5.1 问题：拷进设备 ≠ 已经播放，时间却可能已经加上去了

理想：

```text
get_audio_clock = 耳朵正在听到的媒体时间
```

现实链路：

```text
解码 PCM → memcpy 进 AudioQueue buffer → Enqueue
        → 硬件队列里排队 / 正在播 → 播完才回调
```

**「拷贝到音频设备（Enqueue）」只表示数据进了硬件侧缓冲区，不代表已经从喇叭播出去。**

若在 Enqueue 时就把这一包对应的时长直接当成「已经播过」加进时钟（例如只做 `pts + 已写入字节`，不扣队列里还没播完的），则：

```text
时钟会偏大（超前于真实听到的位置）
视频拿这个偏大的主时钟去追 → 口型/画面容易系统性偏一会儿
```

即便引入水位 `audio_aq_size`（入队 +、播完 −），仍有第二层不准：

- AudioQueue **没有**「盒播到一半」的回调，只有整盒播完才进 `OutputBufferCallback`
- 因此 `wrote` / `consumed` 只能在 **整盒边界** 改账
- 两次回调之间：`cursor`、`aq_size` 不变 → `get_audio_clock` **冻住**
- 真实 DAC 在往前走，时钟要等下次 `consumed` 才跳一下

所以问题可以概括成两句：

1. **语义**：写入设备 ≠ 播放完成；若把「已写入」默认当成「已播放」，时钟会大。  
2. **粒度**：即使扣水位，也只能盒级更新，盒内仍有几十 ms 量级误差。

另外：解码会覆盖 `audio_buf`，软件侧留不住「还在硬件里的上一帧 PCM」，硬件延迟只能靠 **字节账**（或自己存多盒数据），不能幻想从 FFmpeg 里再读出上一包。

### 5.2 本工程方案（折中）

公式：

```text
audio_clock      = 本帧 pts（起点，ms）
audio_buf_cursor = 本帧已 Enqueue 的字节
audio_aq_size    = AQ 中尚未播完的字节（水位，只记账，不存 PCM）

get_audio_clock ≈
    audio_clock
  + cursor 对应时长          // 本帧已交给设备的部分
  − audio_aq_size 对应时长   // 扣掉「进了设备但还没播完」
```

回调时机（关键）：

```text
进 OutputBufferCallback 时，mAudioDataByteSize 仍是「上一盒」写入长度
  → audio_queue_consumed(...)   // 水位 ↓，承认上一盒播完

解码 → memcpy → Enqueue(本盒)
  → audio_queue_wrote(buff_size) // cursor ↑、水位 ↑
```

取舍含义：

| 做了什么 | 没做什么 |
|----------|----------|
| 用水位区分「已拷贝」和「未播完」，减轻「一写入时钟就偏大」 | 不在盒内按采样连续掉水位 |
| 不存多盒 PCM，和「解码即覆盖」兼容 | 两次回调之间时钟冻结 |
| 实现简单，同步大体可用 | 不追求采样级 / 盒内精确 |

**结论：** 这是在复杂度、内存模型与可用精度之间的 **有意折中**——承认「盒级较准、盒内有误差」，不假装已经精确到耳朵。

单位：全程 **ms**；`bytes_per_sec = sample_rate × channels × 2`（S16）。  
`audio_clock == NAN` 时视频侧勿狂追。  
`ref_clock` → `get_master_clock()` → 音频主时钟时即 `get_audio_clock()`。

### 5.3 终极方案（更贴耳，未实现）

目标：任意时刻（含两次 `OutputBufferCallback` 之间）都能估出接近真实播放点的时钟。

常见做法（与 ffplay 一类思路接近）：

1. **仍维护** 队列字节 / 水位（或系统 API 查询已排队未播字节，若平台提供）  
2. 在每次音频回调里记录：  
   - `audio_callback_time`（墙钟）  
   - 当时的媒体时钟基准 `clock_base`  
3. 任意时刻：  

```text
get_audio_clock ≈
    clock_base
  + (now_wall − audio_callback_time)     // 盒内用墙钟往前推
  − （仍排队未播字节 / bytes_per_sec）   // 继续扣硬件延迟
```

并做钳制（不能超过下一帧 pts、暂停时冻结墙钟差等）。

还可选：自己环形缓冲保存「已交给设备未播完」的 PCM 与时间戳，播完再释放——模型最硬，内存与复杂度更高。

终极方案解决的是：**写入 ≠ 播放**（持续扣未播完）+ **盒内冻结**（墙钟插值）。  
本工程未上这套，以免过早引入暂停/追帧/多实例等边界复杂度；当前盒级水位方案作为工程折中保留。

---

## 6. 其它相关注意

- **休眠字段**：`delay_video_time` 与 `sc_delay_ms` 统一用 `double`（ms），只在 `usleep` 边界转微秒；避免 `uint32_t` 把负 delay 转成天文数字
- **送显所有权**：clone 后出队；回调里 `av_frame_free`，不要再次 `fream_queue_pop`
- **大文件**：仓库中的 `libavcodec.a`、样片 mp4 可能超过 GitHub 建议大小，克隆/推送留意 LFS

---

## 结构（简）

```text
SCPlayer_2D/
  player/     # SCPlayer / video / audio / SCRender
  occlass/    # ViewController、AudioQueue 接入
  libs/       # FFmpeg 等
```

入口：`scplayer()`；视频帧通过 `frame_call_bacl` 交给上层渲染。

---

## 7. 近期开发改动总结

### 播放核心

| 项 | 说明 |
|----|------|
| 送显解耦 | 去掉 `frame_display_pending`；`video_display` 里 `av_frame_clone` 后立刻出队，回调只 `av_frame_free` |
| `display_busy` | 放在 `VideoState`，只决定是否送 GL，**不参与** delay / 同步计算；忙则出队丢显 |
| 同步 | 对齐 ffplay：`sync_threshold` 夹 delay，落后 `delay+diff`，超前长帧补 diff、短帧 `2*delay` |
| 休眠 | `delay_video_time` 与 `sc_delay_ms` 统一为 `double`（ms） |
| 换片 | `scplayer_stop` 停旧实例；`clearDisplay` 清黑屏；避免旧帧/旧 busy 残留 |
| 旋转 | 读流 `displaymatrix` / `rotate` → `video_rotate` → `SCRender.rotateDegrees` |
| 网络 | `avformat_network_init`；支持 URL 播放；Info.plist 放开 ATS |

### 渲染 / UI

| 项 | 说明 |
|----|------|
| 全屏 | `SCRender` 铺满屏幕 |
| 控件风格 | 半透明黑底圆角按钮（对齐 GL-ARKit / `SCDropdownButton`） |
| 显隐 | 点屏幕中心区域切换控件栏 |
| `fillMode` | 等比例（letterbox）/ 拉伸铺满 |
| 画质 | 流畅 / 均衡 / 高清 / 超清 → `contentsScale`（超清可高于屏密度，上限 4） |
| 抗锯齿 | 独立档位：关 / 2x / 4x / 8x（按 `GL_MAX_SAMPLES` 封顶）；MSAA FBO + blit 再 present |
| URL | 输入框 +「播放URL」，可播 http(s)/rtmp/rtsp 等 |
| V 暂停 | `vidoe_stop`：只停视频刷新，音频继续；按钮「V暂停 / V播放」 |
| A 暂停 | `SCAudioQueuePlayer` 的 `pause` / `resume`；按钮「A暂停 / A播放」 |
| 同步暂停 | 「暂停 / 播放」同时控视频（`vidoe_stop`）与音频（`pause`/`resume`） |

### 音视频同步测试（现象）

当前刷新走 `video_refresh_timer` → **`video_refresh_timer_tmp`**：

```text
diff = 视频 pts − 音频主时钟
立刻 video_display
再 sc_delay_ms(max(0, diff))
```

源码里另有一套对齐 ffplay 的 `frame_timer` / `sync_threshold` 实现，但被提前 `return`，**未启用**。

**实测现象**：tmp 路径（`delay ≈ pts−audio`，先显示再按 diff 休眠）比未启用的 ffplay 风格路径更稳、口型/画面更跟手；后者在本工程「先 display 再 sleep」的循环模型下，反而更容易卡顿或追不上。故以 tmp 为准，ffplay 段仅作对照保留。

### 接入约定（回调）

1. `flag==1` 收到的是**已克隆**的 `AVFrame*`，用完必须 `av_frame_free`
2. 显示完成必须把 `is->display_busy = 0`
3. 不要在回调里再 `fream_queue_pop`（队已在模块侧出完）

### 局域网直播联调（SRS）

本机 SRS 常见口：`1935` RTMP、`8080` HTTP-FLV、`1985` API。

推流示例（文件环推，自行在终端执行）：

```bash
ffmpeg -re -stream_loop -1 -i "/path/to/video.mp4" \
  -c:v libx264 -preset veryfast -tune zerolatency -pix_fmt yuv420p -g 60 -b:v 4000k \
  -c:a aac -ar 44100 -b:a 128k \
  -f flv rtmp://<局域网IP>/live/desktop
```

播放地址示例：`rtmp://<局域网IP>/live/desktop`  
（桌面采屏推流需给「终端」开 macOS 屏幕录制权限，否则可能被 `killed`。）
