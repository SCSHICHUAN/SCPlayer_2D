# SCPlayer_2D

基于 FFmpeg + OpenGL ES 的 iOS 音视频播放器实验工程（以音频为主时钟做 A/V 同步）。

本文记录实现过程中踩过的坑，方便后续接入或重构时避开。

---

## 1. 音视频同步的坑

同步核心是：**视频跟音频走**（`AV_SYNC_AUDIO_MASTER`）。

```text
diff = 视频帧 pts − 音频时钟
```

### 非异常时，`diff > 0`（视频超前）

直觉上：**再等 `diff` 就可以对齐音频再播**。

但若每次都把等待改成「刚好等于 `diff`」或猛加一大截，画面会一卡一顿。为了**平滑**，不是裸等 `diff`，而是在帧间隔 `delay` 上做修正（对齐 ffplay）：

| 情况 | 做法 | 用意 |
|------|------|------|
| `diff >= sync_threshold` 且 `delay > framedup(≈100ms)` | `delay = delay + diff` | 长帧：把超前量补进等待 |
| `diff >= sync_threshold` 且短帧 | `delay = 2 * delay` | 不一次吃满 `diff`，先拉长一帧显示，慢慢对齐 |
| `\|diff\|` 小于阈值 | 不改 `delay` | 小误差当噪声，避免抖 |

所以：**目标仍是消化掉超前的 `diff`，手段是改 `delay` 做渐进，而不是「睡眠 = diff」一刀切。**

### `diff < 0`（视频落后）

同样为平滑：`delay = max(0, delay + diff)`，缩短等待慢慢追，而不是 delay 直接清零跳播。

阈值建议：`sync_threshold` 夹在约 40ms～100ms，`framedup` 约 100ms；`|diff|` 过大（不连续）则放弃本次修正。

### 常见误区

| 误区 | 后果 |
|------|------|
| 用墙钟硬睡「固定帧间隔」当同步 | 音频一抖视频就漂 |
| `diff > 0` 就 `sleep(diff)`，不做平滑 | 超前时顿挫明显 |
| 把 `display_busy` / GL 完成状态掺进同步计算 | 渲染一慢，整条刷新被拖死 |
| `actual_delay < 某最小值` 就丢帧不显示 | 到点该画的帧反而被扔掉 |
| 启动阶段音频时钟还是 `NAN` 就狂追 | 开头乱丢帧 / 乱加速 |

原则：**同步只认 pts 与主时钟；显示忙只影响「这帧送不送 GL」，不要卡住 `pictq` 推进节奏。**

当前做法：`video_display` 里 `av_frame_clone` 后立刻出队；`display_busy` 时丢弃送显，不参与 `delay` 计算。

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

## 5. 音频时间的计算

**当前播放时刻 = 本帧 pts + 已经消耗掉的时长**（不是「解码到哪就算播到哪」）。

概念上：

```text
play_time = frame_pts + consumed_duration
```

其中 `consumed_duration` 来自：本帧 PCM 里已经交给硬件并播完的部分。

本工程实现上等价写成「帧结束时刻 − 尚未播完」：

```text
audio_clock 存的是：frame_pts + 本帧时长   （本帧结束点）

get_audio_clock ≈
    audio_clock
  − 软件缓冲未读字节耗时
  − AudioQueue 中未播完字节耗时
```

也就是：

```text
结束点 − 剩余未播  ≡  起点 pts + 已消耗
```

坑点：

- 单位要统一（本工程视频/音频时钟均用 **ms**）
- 启动时 `audio_clock` 可能是 `NAN`，视频侧要等时钟建立再追
- 只用 `frame_pts`、不加已消耗 / 不扣未播完，视频会系统性偏快或偏慢
- `bytes_per_sec` 要按**实际输出格式**（如 S16 × 通道 × 采样率），和解码后重采样格式不一致会算歪

视频同步里的 `ref_clock` 应走 `get_master_clock()` → 音频主时钟时即 `get_audio_clock()`。

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
