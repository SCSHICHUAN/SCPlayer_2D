//
//  SCPlayer_VHist.h
//  SCPlayer_2D
//
//  视频补帧（仅 SC_LATENCY_LOW）：环形保存最近 N 个已解码 YUV 帧（默认 60≈1s）。
//  pictq 空时从新到旧克隆补显；须暖机满 5s（见 README §9）。
//  V 暂停追赶期由 video_no_hist_until_catchup 另行禁止补帧。
//  高延时不创建、不调用。
//

#ifndef SCPlayer_VHist_h
#define SCPlayer_VHist_h

#include <libavutil/frame.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SCVideoHist SCVideoHist;

#define SC_VIDEO_HIST_FRAMES 60 /* ~1s @60fps */

SCVideoHist *sc_video_hist_create(void);
void sc_video_hist_destroy(SCVideoHist *h);
void sc_video_hist_reset(SCVideoHist *h);

/* 正常解码入队前：拷贝一帧进历史（内部 av_frame_clone） */
void sc_video_hist_rx(SCVideoHist *h, const AVFrame *frame, double pts_ms, double duration_ms);

/*
 无帧补偿：从新到旧取一帧克隆到 *out_frame（调用方 av_frame_free）。
 成功返回 1；栈空返回 0。调用方须已通过 video_hist_display_once 门闩。
 */
int sc_video_hist_fill(SCVideoHist *h,
                       AVFrame **out_frame,
                       double *out_pts_ms,
                       double *out_duration_ms);

/* 最近一帧时长（ms）；无历史返回 0 */
double sc_video_hist_last_duration_ms(const SCVideoHist *h);

/* 当前有效历史帧数（暖机门闩：须 >0） */
int sc_video_hist_count(const SCVideoHist *h);

#ifdef __cplusplus
}
#endif

#endif /* SCPlayer_VHist_h */
