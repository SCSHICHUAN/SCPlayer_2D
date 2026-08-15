//
//  SCPlayer_PLC.c
//  SCPlayer_2D
//
//  已解码音频历史栈（60 槽≈1s@60fps）；无包时反向取帧（时间反转）补偿。
//

#include "SCPlayer_PLC.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    int16_t *data;
    int      bytes;
    int      nb_samples;
    int      channels;
    int      capacity_bytes;
} SCAudioHistSlot;

struct SCAudioPLC {
    SCAudioHistSlot slots[SC_AUDIO_HIST_FRAMES];
    int count;       /* 有效帧数 0..N */
    int write;       /* 下一写入下标（环形） */
    int play;        /* 补偿读取下标：从新到旧 */
    int concealing;  /* 1=正在无包补偿 */
    int left;        /* 本轮补偿还剩几帧可播；0=已用尽 → 静音 */
    int exhausted;   /* 1=历史已播完，后续填静音直到有真实包 */
};

SCAudioPLC *sc_audio_plc_create(void)
{
    return (SCAudioPLC *)calloc(1, sizeof(SCAudioPLC));
}

void sc_audio_plc_destroy(SCAudioPLC *plc)
{
    int i;
    if (!plc) {
        return;
    }
    for (i = 0; i < SC_AUDIO_HIST_FRAMES; i++) {
        free(plc->slots[i].data);
        plc->slots[i].data = NULL;
    }
    free(plc);
}

void sc_audio_plc_reset(SCAudioPLC *plc)
{
    int i;
    if (!plc) {
        return;
    }
    for (i = 0; i < SC_AUDIO_HIST_FRAMES; i++) {
        plc->slots[i].bytes = 0;
        plc->slots[i].nb_samples = 0;
        plc->slots[i].channels = 0;
    }
    plc->count = 0;
    plc->write = 0;
    plc->play = 0;
    plc->concealing = 0;
    plc->left = 0;
    plc->exhausted = 0;
}

int sc_audio_plc_last_nb_samples(const SCAudioPLC *plc)
{
    int newest;
    if (!plc || plc->count <= 0) {
        return 0;
    }
    newest = (plc->write - 1 + SC_AUDIO_HIST_FRAMES) % SC_AUDIO_HIST_FRAMES;
    return plc->slots[newest].nb_samples;
}

int sc_audio_plc_count(const SCAudioPLC *plc)
{
    return plc ? plc->count : 0;
}

static int sc_hist_slot_store(SCAudioHistSlot *slot,
                              const int16_t *pcm,
                              int nb_samples,
                              int channels)
{
    int bytes;
    if (!slot || !pcm || nb_samples <= 0 || channels <= 0) {
        return -1;
    }
    bytes = nb_samples * channels * (int)sizeof(int16_t);
    if (slot->capacity_bytes < bytes) {
        int16_t *p = (int16_t *)realloc(slot->data, (size_t)bytes);
        if (!p) {
            return -1;
        }
        slot->data = p;
        slot->capacity_bytes = bytes;
    }
    memcpy(slot->data, pcm, (size_t)bytes);
    slot->bytes = bytes;
    slot->nb_samples = nb_samples;
    slot->channels = channels;
    return 0;
}

/* 交织 S16：按采样点时间反转（多声道保持一帧内通道顺序） */
static void sc_hist_reverse_copy(const int16_t *src,
                                 int nb_samples,
                                 int channels,
                                 int16_t *dst)
{
    int i, c;
    for (i = 0; i < nb_samples; i++) {
        int src_i = nb_samples - 1 - i;
        for (c = 0; c < channels; c++) {
            dst[i * channels + c] = src[src_i * channels + c];
        }
    }
}

void sc_audio_plc_rx(SCAudioPLC *plc,
                     const int16_t *interleaved,
                     int nb_samples,
                     int channels,
                     int sample_rate)
{
    (void)sample_rate;
    if (!plc || !interleaved || nb_samples <= 0 || channels <= 0) {
        return;
    }
    if (sc_hist_slot_store(&plc->slots[plc->write], interleaved, nb_samples, channels) < 0) {
        return;
    }
    plc->write = (plc->write + 1) % SC_AUDIO_HIST_FRAMES;
    if (plc->count < SC_AUDIO_HIST_FRAMES) {
        plc->count++;
    }
    /* 新真实包到来：重置补偿状态 */
    plc->concealing = 0;
    plc->left = 0;
    plc->exhausted = 0;
    plc->play = (plc->write - 1 + SC_AUDIO_HIST_FRAMES) % SC_AUDIO_HIST_FRAMES;
}

int sc_audio_plc_fill(SCAudioPLC *plc,
                      int16_t *out_interleaved,
                      int nb_samples,
                      int channels,
                      int sample_rate)
{
    SCAudioHistSlot *slot;
    int want_bytes;
    int copy_samples;

    (void)sample_rate;
    if (!plc || !out_interleaved || nb_samples <= 0 || channels <= 0) {
        return 0;
    }
    want_bytes = nb_samples * channels * (int)sizeof(int16_t);

    if (plc->count <= 0 || plc->exhausted) {
        memset(out_interleaved, 0, (size_t)want_bytes);
        return want_bytes;
    }

    if (!plc->concealing) {
        /* 进入补偿：从最新一帧往旧播，用尽后静音 */
        plc->play = (plc->write - 1 + SC_AUDIO_HIST_FRAMES) % SC_AUDIO_HIST_FRAMES;
        plc->left = plc->count;
        plc->concealing = 1;
    }

    if (plc->left <= 0) {
        plc->exhausted = 1;
        memset(out_interleaved, 0, (size_t)want_bytes);
        return want_bytes;
    }

    slot = &plc->slots[plc->play];
    if (!slot->data || slot->nb_samples <= 0 || slot->channels <= 0) {
        plc->exhausted = 1;
        plc->left = 0;
        memset(out_interleaved, 0, (size_t)want_bytes);
        return want_bytes;
    }

    /* 通道不一致：静音兜底 */
    if (slot->channels != channels) {
        memset(out_interleaved, 0, (size_t)want_bytes);
    } else {
        copy_samples = slot->nb_samples < nb_samples ? slot->nb_samples : nb_samples;
        sc_hist_reverse_copy(slot->data, copy_samples, channels, out_interleaved);
        if (copy_samples < nb_samples) {
            memset(out_interleaved + copy_samples * channels,
                   0,
                   (size_t)(nb_samples - copy_samples) * channels * sizeof(int16_t));
        }
    }

    plc->left--;
    if (plc->left > 0) {
        plc->play = (plc->play - 1 + SC_AUDIO_HIST_FRAMES) % SC_AUDIO_HIST_FRAMES;
    } else {
        /* 历史播完 → 之后静音，直到有真实音频 */
        plc->exhausted = 1;
    }

    return want_bytes;
}
