//
//  SCPlayer_PLC.h
//  SCPlayer_2D
//
//  音频补帧（仅 SC_LATENCY_LOW）：环形保存最近 N 个已解码 S16 帧（默认 60≈1s）。
//  无包时从新到旧取出并样点时间反转；须暖机满 5s 且已有历史（见 README §9）。
//  高延时不创建、不调用。
//

#ifndef SCPlayer_PLC_h
#define SCPlayer_PLC_h

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SCAudioPLC SCAudioPLC;

#define SC_AUDIO_HIST_FRAMES 60 /* ~1s @60fps 槽位 */

SCAudioPLC *sc_audio_plc_create(void);
void sc_audio_plc_destroy(SCAudioPLC *plc);
void sc_audio_plc_reset(SCAudioPLC *plc);

/* 正常解码出的 S16 交织 PCM：压入历史栈（满则覆盖最旧）；真实包到来时重置 concealing */
void sc_audio_plc_rx(SCAudioPLC *plc,
                     const int16_t *interleaved,
                     int nb_samples,
                     int channels,
                     int sample_rate);

/*
 无包补偿：从栈顶向旧方向取一帧，样点反转后写入 out。
 栈空/用尽则填静音。返回写出字节数；失败 0。
 调用方须已通过暖机门闩（audio_reuse_last_buf）。
 */
int sc_audio_plc_fill(SCAudioPLC *plc,
                      int16_t *out_interleaved,
                      int nb_samples,
                      int channels,
                      int sample_rate);

/* 历史里最近一帧的采样数（用于对齐 AQ 盒长）；无历史返回 0 */
int sc_audio_plc_last_nb_samples(const SCAudioPLC *plc);

/* 当前有效历史帧数（暖机门闩：须 >0） */
int sc_audio_plc_count(const SCAudioPLC *plc);

#ifdef __cplusplus
}
#endif

#endif /* SCPlayer_PLC_h */
