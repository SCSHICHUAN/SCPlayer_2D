//
//  SCPlayer_VHist.c
//  SCPlayer_2D
//
//  已解码视频历史（60 槽≈1s@60fps）；无帧时从新到旧取克隆补显。
//

#include "SCPlayer_VHist.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct {
    AVFrame *frame;
    double   pts_ms;
    double   duration_ms;
} SCVideoHistSlot;

struct SCVideoHist {
    SCVideoHistSlot slots[SC_VIDEO_HIST_FRAMES];
    int count;
    int write;
    int play;
    int concealing;
};

SCVideoHist *sc_video_hist_create(void)
{
    return (SCVideoHist *)calloc(1, sizeof(SCVideoHist));
}

void sc_video_hist_destroy(SCVideoHist *h)
{
    int i;
    if (!h) {
        return;
    }
    for (i = 0; i < SC_VIDEO_HIST_FRAMES; i++) {
        av_frame_free(&h->slots[i].frame);
    }
    free(h);
}

void sc_video_hist_reset(SCVideoHist *h)
{
    int i;
    if (!h) {
        return;
    }
    for (i = 0; i < SC_VIDEO_HIST_FRAMES; i++) {
        av_frame_unref(h->slots[i].frame);
        h->slots[i].pts_ms = NAN;
        h->slots[i].duration_ms = 0;
    }
    h->count = 0;
    h->write = 0;
    h->play = 0;
    h->concealing = 0;
}

double sc_video_hist_last_duration_ms(const SCVideoHist *h)
{
    int newest;
    if (!h || h->count <= 0) {
        return 0;
    }
    newest = (h->write - 1 + SC_VIDEO_HIST_FRAMES) % SC_VIDEO_HIST_FRAMES;
    return h->slots[newest].duration_ms;
}

int sc_video_hist_count(const SCVideoHist *h)
{
    return h ? h->count : 0;
}

void sc_video_hist_rx(SCVideoHist *h, const AVFrame *frame, double pts_ms, double duration_ms)
{
    SCVideoHistSlot *slot;
    AVFrame *clone;

    if (!h || !frame) {
        return;
    }
    clone = av_frame_clone(frame);
    if (!clone) {
        return;
    }

    slot = &h->slots[h->write];
    av_frame_free(&slot->frame);
    slot->frame = clone;
    slot->pts_ms = pts_ms;
    slot->duration_ms = duration_ms > 0 ? duration_ms : 0;

    h->write = (h->write + 1) % SC_VIDEO_HIST_FRAMES;
    if (h->count < SC_VIDEO_HIST_FRAMES) {
        h->count++;
    }
    h->concealing = 0;
    h->play = (h->write - 1 + SC_VIDEO_HIST_FRAMES) % SC_VIDEO_HIST_FRAMES;
}

int sc_video_hist_fill(SCVideoHist *h,
                       AVFrame **out_frame,
                       double *out_pts_ms,
                       double *out_duration_ms)
{
    SCVideoHistSlot *slot;
    AVFrame *clone;

    if (!h || !out_frame) {
        return 0;
    }
    *out_frame = NULL;
    if (h->count <= 0) {
        return 0;
    }

    if (!h->concealing) {
        h->play = (h->write - 1 + SC_VIDEO_HIST_FRAMES) % SC_VIDEO_HIST_FRAMES;
        h->concealing = 1;
    }

    slot = &h->slots[h->play];
    if (!slot->frame) {
        return 0;
    }
    clone = av_frame_clone(slot->frame);
    if (!clone) {
        return 0;
    }

    *out_frame = clone;
    if (out_pts_ms) {
        *out_pts_ms = slot->pts_ms;
    }
    if (out_duration_ms) {
        *out_duration_ms = slot->duration_ms;
    }

    if (h->count > 1) {
        int oldest = (h->write - h->count + SC_VIDEO_HIST_FRAMES) % SC_VIDEO_HIST_FRAMES;
        if (h->play != oldest) {
            h->play = (h->play - 1 + SC_VIDEO_HIST_FRAMES) % SC_VIDEO_HIST_FRAMES;
        }
    }
    return 1;
}
