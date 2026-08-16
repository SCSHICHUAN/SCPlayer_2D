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

即便引入水位 `audio_aq_size`（入队记账），仍有第二层不准：

- AudioQueue **没有**「盒播到一半」的回调，接点只在整盒边界（本工程用两次 `wrote`）
- 若只在接点改账：两次回调之间时钟会**冻住**
- 因此需要在两次 `wrote` 之间做线性插值补进度

所以问题可以概括成两句：

1. **语义**：写入设备 ≠ 播放完成；若把「已写入」默认当成「已播放」，时钟会大。  
2. **粒度**：即使扣水位，也只能盒级更新，盒内仍有几十 ms 量级误差。

另外：解码会覆盖 `audio_buf`，软件侧留不住「还在硬件里的上一帧 PCM」，硬件延迟只能靠 **字节账**（或自己存多盒数据），不能幻想从 FFmpeg 里再读出上一包。

### 5.2 本工程方案（wrote 重置 + 线性时钟）

公式：

```text
解码 → audio_write_pts = 帧 pts（不进主时钟）
wrote(flag 2) → 重置：audio_clock = write_pts，记下墙钟与本盒 size
get_audio_clock = audio_clock + min(墙钟差, 本盒时长)   // 0~size 线性
```

只认 flag 2。视频 `tmp` 路径：算 diff 后送显，再用 `delay_video_time` 控制下一轮节奏。暂停时冻结音频已流逝时长。

单位：全程 **ms**；`bytes_per_sec = sample_rate × channels × 2`（S16）。  
`audio_clock == NAN` 时视频侧勿狂追。  
`ref_clock` → `get_master_clock()` → 音频主时钟时即 `get_audio_clock()`。

### 5.3 更贴耳的扩展（可选）

若还要再贴耳，可在回调里记 `clock_base` + `audio_callback_time`，读时钟时用墙钟差往前推，并对暂停/追帧做钳制；或环形缓冲保存未播完 PCM。当前线性水位已覆盖「盒内冻结」的主要痛点。

---

## 6. AudioQueue 与 SCPlayer 解耦

### 以前的问题

`SCAudioQueuePlayer` 直接 `#include "SCPlayer.h"`，在回调里摸：

- `scp->audioq` / `audio_buf` / `out_audio_size`
- `audio_decode_callback` / `audio_queue_wrote` / `audio_queue_consumed`

设备层和播放内核缠在一起：换输出设备、单测 AudioQueue、或内核改字段都要改 OC 播放器。

### 现在的做法

`SCAudioQueuePlayer` **只负责** AudioQueue（建队列、盒回调、memcpy、Enqueue、pause/resume）。  
业务通过函数指针回调，由接入方（如 `ViewController`）对接 `SCPlayer`：

```text
flag 0：解码取 PCM → 出参 outPCM / outSize
        返回 0 有数据 / 1 暂无填静音 / -1 结束停播
flag 2：本盒已 Enqueue → wrote(len)：入队 + 对齐 + 开插值
flag 1：保留编号，设备层不再调用

初始化：只传 sampleRate / channels / userData，不传 SCPlayer 类型
```

### 解耦的好处

| 好处 | 说明 |
|------|------|
| 职责清晰 | Queue = 设备与缓冲；SCPlayer = 解码与时钟水位；VC = 粘合 |
| 编译隔离 | `SCAudioQueuePlayer` 不再依赖 `SCPlayer.h` / FFmpeg 头，改内核字段不必重编设备层语义 |
| 可替换 | 同一套回调可接别的 PCM 源，或把 AudioQueue 换成其他输出，内核不动 |
| 可测 | 可用假回调喂静音/固定 PCM，单独验证 Enqueue / pause，不必拉起整条解复用 |
| 与视频侧对称 | 视频已是 `frame_call_bacl` 送显；音频同样「模块回调、接入方实现」，边界一致 |
| 水位仍在内核 | `wrote` 插值/对齐留在 `SCPlayer_audio`，时钟公式不泄漏进 OC |

注意：初始化前必须先设好回调；`initialize` 会立刻预填若干 buffer 并触发回调。

---

## 7. 其它相关注意

- **休眠字段**：`delay_video_time` 与 `sc_delay_ms` 统一用 `double`（ms），只在 `usleep` 边界转微秒；避免 `uint32_t` 把负 delay 转成天文数字
- **送显所有权**：clone 后出队；回调里 `av_frame_free`，不要再次 `fream_queue_pop`
- **大文件**：仓库中的 `libavcodec.a`、样片 mp4 可能超过 GitHub 建议大小，克隆/推送留意 LFS

### 7.1 冷启动音频爆破音（AVAudioSession）

**现象**：杀进程后第一次点播放有「啪/爆」一声；播起来后再换片则正常。

**原因**：冷启动时 `AVAudioSession` 尚未 `setActive`，首帧 `AudioQueueStart` 才同时激活 session、拉起 DAC，并立刻送 PCM。硬件从静音突然接到有幅度波形 → 爆音。换片时 session 已热，只是换数据源，故正常。

**处理（少量代码）**：进播前预热一次即可：

```text
[SCAudioQueuePlayer warmUpAudioSession]
  → setCategory(Playback) + setActive(YES)   // dispatch_once
```

当前在 `ViewController.viewDidLoad` 调用。与 A/V 同步、时钟公式无关。

### 7.2 暂停后再选片卡死（刷新线程 join）

**现象**：点「暂停」（或 V 暂停使 `vidoe_stop=1`）后再选视频，主线程卡住。

**原因**：

```text
选片 → stopCurrentPlayback → scplayer_stop
     → quit=1 → pthread_join(video_loop)
```

若 `video_refresh_loop` 在 `vidoe_stop==1` 时只 `sleep` / `continue`、**不查 `quit`**，刷新线程永远不退出，`join` 死等 → UI 卡死。

**处理**：循环每轮**先查 `quit` 再处理暂停**：

```text
for (;;) {
    if (quit) break;
    if (vidoe_stop) { sleep 10ms; continue; }
    refresh + delay...
}
```

---

## 结构（简）

```text
SCPlayer_2D/
  player/     # SCPlayer / video / audio / SCRender
  occlass/    # ViewController、AudioQueue 接入
  libs/       # FFmpeg 等
```

入口：`scplayer()`；视频帧通过 `frame_call_bacl` 交给上层渲染；音频设备通过 `Audion_queue_call_other` 向接入方要 PCM。

---

## 8. 近期开发改动总结

### 播放核心

| 项 | 说明 |
|----|------|
| 送显解耦 | 去掉 `frame_display_pending`；`video_display` 里 `av_frame_clone` 后立刻出队，回调只 `av_frame_free` |
| `display_busy` | 放在 `VideoState`，只决定是否送 GL，**不参与** delay / 同步计算；忙则出队丢显 |
| 音频设备解耦 | `SCAudioQueuePlayer` 不依赖 `SCPlayer`；经 `Audion_queue_call_other` 取 PCM / wrote（插值与对齐） |
| 同步 | 对齐 ffplay：`sync_threshold` 夹 delay，落后 `delay+diff`，超前长帧补 diff、短帧 `2*delay` |
| 休眠 | `delay_video_time` 与 `sc_delay_ms` 统一为 `double`（ms） |
| 换片 | `scplayer_stop` 停旧实例；`clearDisplay` 清黑屏；避免旧帧/旧 busy 残留 |
| 暂停+换片 | `video_refresh_loop` 先查 `quit` 再处理 `vidoe_stop`，避免 `join` 死锁（§7.2） |
| 冷启动音频 | `warmUpAudioSession` 进播前激活 session，消首播爆破音（§7.1） |
| 高低延时 | `SCLatencyMode` + 补帧暖机 5s / V 暂停追赶门闩，见 **§9** |
| 音频队列滞回 | `hysteresis_samples`：积压用回差区间，避免临界点来回切速度（§9.3） |
| 补偿回落 | 纠偏时逐步扣减 A/V `compensation_pts`，防累计导致永不同步（§9.3） |
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

---

## 9. 高低延时与 A/V 补帧

UI「低延时 / 高延时」写入 `scp->latency_mode`（`SCLatencyMode`），并映射同步类型；**补偿逻辑只认 `latency_mode`，高延时路径不跑补帧。**

| UI | `latency_mode` | `av_sync_type` | A/V 历史补帧 |
|----|----------------|----------------|--------------|
| 低延时 | `SC_LATENCY_LOW` | `AV_SYNC_EXTERNAL_MASTER` | 有（暖机后） |
| 高延时 | `SC_LATENCY_HIGH` | `AV_SYNC_AUDIO_MASTER` | **无**（无包填静音 / pictq 空只等） |

入口：`set_latency_mode()`；运行时以 `sc_av_compensate_enabled(scp)` 为准。

### 9.1 低延时时钟关系

低延时不是「音视频都直接跟墙钟」，而是：

```text
外部钟（墙钟铆点）
    ↓ 纠偏 / 加速消耗积压
音频（跟外部钟）
    ↓ get_audio_clock
视频（跟音频钟）
```

| 时钟 | 含义 |
|------|------|
| `external` | 首次音频 `wrote` 时钉墙钟铆点；`get = now − 铆点`；之后不被音频反复拉回 |
| `audio` | `get_audio_clock = audio_clock + 本包已播时长`（wrote 重置基点） |
| `video` | 刷新时 `diff = 视频帧 pts − get_audio_clock()`，再 `delay ≈ max(0, diff)` |

要点：**外部钟只给音频纠偏用；视频始终认音频钟**。音频补偿与视频补偿**互不交叉累加**（避免双计）。

### 9.2 开播暖机（5 秒）

自**首次真实播放**起算墙钟（首次低延时 `wrote` 或首次 `pictq` 送显钉 `compensate_warm_start_ms`）：

- **未满 `SC_COMPENSATE_WARM_MS`（5000ms）**：不补帧、不累加 `audio/video_compensation_pts`；音频侧返回 0 由上层填静音且不 `wrote`。
- **满 5s 后**：才允许 PLC / 视频历史补帧（仍须已有历史、视频已送显过等条件）。

避免开播阶段空队列被当成断流，把补偿从 0 抬高。

### 9.3 音频纠偏与补帧（仅低延时）

#### 积压加速：滞回，避免临界点来回切换

问题：队列深度在某个阈值（如 0.02MB）上下抖时，若每次回调都在「原速 / 加速」之间切换 → **atempo 来回切，声音怪异**（发闷、发飘、节奏发颤）。

做法（`synchronize_audio_to_external` + `hysteresis_samples`）：用**回差带**，只在越过边界时改速度，区间内保持上一档：

```text
audioq 大小 (MB)
  ≤ 0.01     → 原速（hysteresis = nb_samples），让队列慢慢涨
  ≥ 0.03     → 约 +10% 加速（少吐 10%）
  ≥ 0.05     → 约 +50% 加速（atempo 上限约 2×）
  (0.01,0.03) → 不改档，沿用 hysteresis_samples
```

对应提交：`回置区间 [0.01 0.03] …` —— 解决临界点来回切换导致音视频听感/观感怪异。

#### 补偿累计回落：防纠偏偏离

问题：断流补帧把 `audio/video_compensation_pts` 抬高后，若一直加在真实 pts 上 → **时间轴被永久抬偏，音视频像永远对不齐**。

做法：在每次外部纠偏路径里，若累计 > 0 则每次约减 10ms，逐步回到 0：

```c
/* 纠偏后回落，防止补偿导致永远不同步 */
if (video_compensation_pts > 0) video_compensation_pts -= 10;
if (audio_compensation_pts > 0) audio_compensation_pts -= 10;
```

对应提交：`fix:防止纠偏导致音视频永远不同步`。

#### 无包补帧

触发：`audio_decode_frame_external` 在 `audioq` 空 / 取包失败 → `audio_reuse_last_buf`。

1. **无包且暖机完成**：PCM 历史栈（约 60 槽≈1s）从新到旧 + 样点反转；用尽后静音，仍可短暂累加 `audio_compensation_pts`（随后按上面回落）。
2. **高延时**：`audio_decode_frame_audio`，无包上层静音，无 PLC / 无补偿。

### 9.4 视频补帧（仅低延时）

触发：`video_refresh_timer_external_clock` 在 `pictq.size==0` → `video_hist_display_once`。

1. 正常：解码入 `pictq`，同时 `sc_video_hist_rx`；入队 pts 含 `video_compensation_pts`。
2. `pictq` 空且暖机完成：历史从新到旧克隆补显，累加 `video_compensation_pts`。
3. **V 暂停中**：不补帧（`vidoe_stop`）。
4. **V 暂停 → 再播放**（`video_no_hist_until_catchup`）：仅当 **视频 pts ≥ 音频钟** 后才再允许补帧；追上前只追真实 `pictq`，防止狂补时间。  
   **未做该测试时标志为 0**，暖机后照常补帧（不要用「平常也要求 video≥audio」否则送显后几乎永补不了）。
5. **高延时**：`video_refresh_timer_audio_clock`，空队列只 `delay=1` 再探，无 hist。

### 9.5 视频刷新循环

```text
video_refresh_loop（独立 pthread）
  for (;;) {
      if (quit) break;
      if (vidoe_stop) { sleep 10ms; continue; }   // 只停画面，每轮仍查 quit
      video_refresh_timer(scp);                  // 按 latency_mode 分高/低路径
      sc_delay_ms(delay_video_time);
  }
```

| 点 | 说明 |
|----|------|
| 自己控节奏 | `delay_video_time` 由上一轮 pts vs 音频钟（或补帧时的 `frame_duration`）决定 |
| 与解码 / 渲染分离 | 解码填 `pictq`；刷新 clone 送 GL 后立刻出队 |
| 暂停 | `vidoe_stop` 分支也必须查 `quit`，否则换片 `join` 会死锁（见 §7.2） |

### 9.6 最近两次提交（问题 → 解法）

| 提交 | 现象 | 根因 | 解法 |
|------|------|------|------|
| `回置区间 [0.01 0.03]…` | 声音怪异（发颤/发闷），像音视频节奏不稳 | `audioq` 深度在临界点上下抖 → 原速/加速**来回切换**，atempo 跟着抖 | `hysteresis_samples` 回差：≤0.01MB 原速、≥0.03 加速；中间不改档 |
| `fix:防止纠偏导致音视频永远不同步` | 补帧后纠偏偏离，音画像永远对不齐 | `audio/video_compensation_pts` **只增不减**，pts 被永久抬高 | 纠偏路径里每次把累计补偿约 **-10ms**，逐步回到 0 |

一句话：**低延时 = 外部钟纠音频、音频钟带视频 + 暖机 5s 后补帧；积压用滞回防临界切换；补偿要回落防永久偏离。**
