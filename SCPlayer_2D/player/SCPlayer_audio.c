//
//  SCPlayer_audio.c
//  SCPlayer_2D
//
//  Created by Stan on 2026/8/3.
//  Copyright © 2026 石川. All rights reserved.
//

#include "SCPlayer_audio.h"
#include "SCPlayer_sync.h"
#include <math.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libavutil/opt.h>

#define SC_SAMPLE_CORRECTION_PERCENT_MAX 10

static void sc_atempo_close(SCPlayer *scp);

static int audio_bytes_per_sec(SCPlayer *scp)
{
    if (!scp->audio_ctx) {
        return 0;
    }
    return scp->audio_ctx->sample_rate * scp->audio_ctx->ch_layout.nb_channels * 2;
}

/* 自上次 wrote 以来的线性时长（ms），钳在 0~本盒时长 */
static double audio_lin_elapsed_ms_l(SCPlayer *scp)
{
    int bps;
    double elapsed;
    double max_ms;

    if (!scp->audio_lin_ready) {
        return 0;
    }
    if (scp->audio_lin_paused) {
        return scp->audio_lin_elapsed_ms;
    }

    elapsed = av_gettime_ms() - scp->audio_lin_wall_ms;
    if (elapsed < 0) {
        elapsed = 0;
    }

    bps = audio_bytes_per_sec(scp);
    if (bps > 0 && scp->audio_lin_bytes > 0) {
        max_ms = sc_sec_to_ms((double)scp->audio_lin_bytes / (double)bps);
        if (elapsed > max_ms) {
            elapsed = max_ms;
        }
    }
    return elapsed;
}

void audio_aq_interp_start(SCPlayer *scp)
{
    if (!scp) {
        return;
    }
    pthread_mutex_init(&scp->audio_clock_mutex, NULL);
    scp->audio_clock = NAN;
    scp->audio_write_pts = NAN;
    scp->audio_lin_wall_ms = 0;
    scp->audio_lin_bytes = 0;
    scp->audio_lin_ready = 0;
    scp->audio_lin_paused = 0;
    scp->audio_lin_elapsed_ms = 0;
    if (scp->audio_ctx && scp->audio_ctx->sample_rate > 0) {
        scp->audio_diff_threshold_ms =
            sc_sec_to_ms(2.0 * (double)AUDIO_BUFFER_SIZE / (double)scp->audio_ctx->sample_rate);
    } else {
        scp->audio_diff_threshold_ms = 50.0;
    }
}

void audio_aq_interp_stop(SCPlayer *scp)
{
    if (!scp) {
        return;
    }
    sc_atempo_close(scp);
    pthread_mutex_destroy(&scp->audio_clock_mutex);
    scp->audio_lin_ready = 0;
}

void audio_aq_set_paused(SCPlayer *scp, int paused)
{
    if (!scp) {
        return;
    }
    pthread_mutex_lock(&scp->audio_clock_mutex);
    if (paused && !scp->audio_lin_paused) {
        scp->audio_lin_elapsed_ms = audio_lin_elapsed_ms_l(scp);
        scp->audio_lin_paused = 1;
    } else if (!paused && scp->audio_lin_paused) {
        /* 恢复：把墙钟起点挪到「等效已流逝」处 */
        scp->audio_lin_wall_ms = av_gettime_ms() - scp->audio_lin_elapsed_ms;
        scp->audio_lin_paused = 0;
    }
    pthread_mutex_unlock(&scp->audio_clock_mutex);
}

/* 始终建 swr→S16，EXTERNAL 才能 swr_set_compensation */
void init_sws_audio(SCPlayer *scp){
    AVChannelLayout in_ch_layout, out_ch_layout;
    if (scp->audio_swr_ctx) {
        return;
    }
    av_channel_layout_copy(&in_ch_layout, &scp->audio_ctx->ch_layout);
    av_channel_layout_copy(&out_ch_layout, &in_ch_layout);
    swr_alloc_set_opts2(&scp->audio_swr_ctx,
                        &out_ch_layout,
                        AV_SAMPLE_FMT_S16,
                        scp->audio_ctx->sample_rate,
                        &in_ch_layout,
                        scp->audio_ctx->sample_fmt,
                        scp->audio_ctx->sample_rate,
                        0,
                        NULL);
    swr_init(scp->audio_swr_ctx);
}

static void sc_atempo_close(SCPlayer *scp)
{
    if (!scp) {
        return;
    }
    avfilter_graph_free(&scp->audio_filter_graph);
    scp->audio_buffersrc_ctx = NULL;
    scp->audio_buffersink_ctx = NULL;
    scp->audio_filter_tempo = 1.0;
}

/* abuffer -> atempo -> abuffersink，S16 进出，变速不变调 */
static int sc_atempo_open(SCPlayer *scp, int sample_rate, AVChannelLayout *ch_layout, double tempo)
{
    char args[256];
    char ch_desc[128];
    int ret;
    const AVFilter *abuffersrc;
    const AVFilter *abuffersink;
    const AVFilter *atempo;
    AVFilterContext *atempo_ctx = NULL;
    AVFilterGraph *graph;

    sc_atempo_close(scp);
    if (tempo < 0.5) {
        tempo = 0.5;
    }
    if (tempo > 2.0) {
        tempo = 2.0;
    }

    graph = avfilter_graph_alloc();
    if (!graph) {
        return AVERROR(ENOMEM);
    }

    abuffersrc = avfilter_get_by_name("abuffer");
    abuffersink = avfilter_get_by_name("abuffersink");
    atempo = avfilter_get_by_name("atempo");
    if (!abuffersrc || !abuffersink || !atempo) {
        avfilter_graph_free(&graph);
        return AVERROR_FILTER_NOT_FOUND;
    }

    av_channel_layout_describe(ch_layout, ch_desc, sizeof(ch_desc));
    snprintf(args, sizeof(args),
             "time_base=1/%d:sample_rate=%d:sample_fmt=s16:channel_layout=%s",
             sample_rate, sample_rate, ch_desc);

    ret = avfilter_graph_create_filter(&scp->audio_buffersrc_ctx, abuffersrc, "in",
                                       args, NULL, graph);
    if (ret < 0) {
        goto fail;
    }

    {
        char tempo_args[64];
        snprintf(tempo_args, sizeof(tempo_args), "tempo=%f", tempo);
        ret = avfilter_graph_create_filter(&atempo_ctx, atempo, "atempo",
                                           tempo_args, NULL, graph);
        if (ret < 0) {
            goto fail;
        }
    }

    ret = avfilter_graph_create_filter(&scp->audio_buffersink_ctx, abuffersink, "out",
                                       NULL, NULL, graph);
    if (ret < 0) {
        goto fail;
    }

    {
        enum AVSampleFormat out_fmts[] = { AV_SAMPLE_FMT_S16, AV_SAMPLE_FMT_NONE };
        ret = av_opt_set_int_list(scp->audio_buffersink_ctx, "sample_fmts", out_fmts,
                                  AV_SAMPLE_FMT_NONE, AV_OPT_SEARCH_CHILDREN);
        if (ret < 0) {
            goto fail;
        }
    }

    ret = avfilter_link(scp->audio_buffersrc_ctx, 0, atempo_ctx, 0);
    if (ret < 0) {
        goto fail;
    }
    ret = avfilter_link(atempo_ctx, 0, scp->audio_buffersink_ctx, 0);
    if (ret < 0) {
        goto fail;
    }

    ret = avfilter_graph_config(graph, NULL);
    if (ret < 0) {
        goto fail;
    }

    scp->audio_filter_graph = graph;
    scp->audio_filter_tempo = tempo;
    return 0;

fail:
    avfilter_graph_free(&graph);
    scp->audio_buffersrc_ctx = NULL;
    scp->audio_buffersink_ctx = NULL;
    return ret;
}

/*
 对已是 S16 交织的 PCM 做 atempo。
 tempo>1 加快（输出变短），音色/音高保持。
 返回输出字节数，失败 <0。
 */
static int sc_atempo_process_s16(SCPlayer *scp, uint8_t *pcm, int nb_samples,
                                 int channels, int sample_rate, double tempo,
                                 uint8_t **out_pcm)
{
    AVFrame *in_frame = NULL;
    AVFrame *out_frame = NULL;
    int ret;
    int out_bytes = 0;
    unsigned int malloc_size = 0;
    AVChannelLayout ch = {0};

    if (!scp || !pcm || nb_samples <= 0) {
        return -1;
    }
    if (fabs(tempo - 1.0) < 0.001) {
        return nb_samples * channels * 2;
    }

    av_channel_layout_default(&ch, channels);
    if (!scp->audio_filter_graph || fabs(scp->audio_filter_tempo - tempo) > 0.001) {
        ret = sc_atempo_open(scp, sample_rate, &ch, tempo);
        if (ret < 0) {
            av_channel_layout_uninit(&ch);
            return ret;
        }
    }

    in_frame = av_frame_alloc();
    out_frame = av_frame_alloc();
    if (!in_frame || !out_frame) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    in_frame->format = AV_SAMPLE_FMT_S16;
    in_frame->sample_rate = sample_rate;
    in_frame->nb_samples = nb_samples;
    ret = av_channel_layout_copy(&in_frame->ch_layout, &ch);
    if (ret < 0) {
        goto end;
    }
    ret = av_frame_get_buffer(in_frame, 0);
    if (ret < 0) {
        goto end;
    }
    memcpy(in_frame->data[0], pcm, (size_t)nb_samples * channels * 2);

    ret = av_buffersrc_add_frame_flags(scp->audio_buffersrc_ctx, in_frame, 0);
    if (ret < 0) {
        goto end;
    }

    ret = av_buffersink_get_frame(scp->audio_buffersink_ctx, out_frame);
    if (ret < 0) {
        goto end;
    }

    out_bytes = out_frame->nb_samples * channels * 2;
    av_fast_malloc(out_pcm, &malloc_size, out_bytes + 32);
    if (!*out_pcm) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    memcpy(*out_pcm, out_frame->data[0], out_bytes);
    ret = out_bytes;

end:
    av_frame_free(&in_frame);
    av_frame_free(&out_frame);
    av_channel_layout_uninit(&ch);
    return ret;
}


/*
 有未消耗完的包 → 强制少吐采样加快消耗（靠队列深度，不单靠时钟）。
 */
static int sc_synchronize_audio_to_ext(SCPlayer *scp, int nb_samples)
{
    int wanted;
    double diff;
    double ext;
    int min_nb;
    int freq;

    if (!scp || nb_samples <= 0) {
        return nb_samples;
    }
    /* 没有未消耗完的包：原速 */
    if (scp->audioq.nb_packets <= 0) {
        return nb_samples;
    }

    freq = scp->audio_ctx ? scp->audio_ctx->sample_rate : 0;
    if (freq <= 0) {
        return nb_samples;
    }

    min_nb = nb_samples * (100 - SC_SAMPLE_CORRECTION_PERCENT_MAX) / 100;
    /* 队列没消耗干净：直接最大加速 */
    wanted = min_nb;

    ext = sc_ext_clock_get(scp);
    if (!isnan(ext)) {
        diff = get_audio_clock(scp) - ext;
        if (!isnan(diff) && diff < 0 &&
            fabs(diff) >= scp->audio_diff_threshold_ms &&
            fabs(diff) < SC_NOSYNC_THRESHOLD_MS) {
            int by_diff = nb_samples + (int)(diff / SC_S_TO_MS * (double)freq);
            if (by_diff < wanted) {
                wanted = by_diff;
            }
            if (wanted < min_nb) {
                wanted = min_nb;
            }
        }
    }

    if (wanted > nb_samples) {
        wanted = nb_samples;
    }
    if (wanted < 1) {
        wanted = 1;
    }
    return wanted;
}

/* 无包：复用上一盒，并把 write_pts 按本盒时长往前推 */
static int audio_reuse_last_buf(SCPlayer *scp)
{
    int bps;

    if (!scp->audio_buf || scp->audio_buf_size == 0) {
        return 0;
    }
    bps = audio_bytes_per_sec(scp);
    if (bps > 0 && !isnan(scp->audio_write_pts)) {
        scp->audio_write_pts += sc_sec_to_ms((double)scp->audio_buf_size / (double)bps);
    }
    scp->audio_buf_cursor = 0;
    return (int)scp->audio_buf_size;
}

//=========================== 解码音频包保存到音频帧到队列中 ===========================
static int audio_decode_frame_audio(SCPlayer *scp)
{
    int ret = -1;
    int len2;
    int data_size = 0;

    for (;;){
        if (packet_queue_get(&scp->audioq, &scp->audio_pkt, 1) < 0){
            av_log(NULL,AV_LOG_ERROR,"不能从音频包queue中读取取包!\n");
            scp->quit = 1;
            break;
        }

        ret = avcodec_send_packet(scp->audio_ctx, &scp->audio_pkt);
        if (ret < 0){
            av_log(scp->audio_ctx, AV_LOG_ERROR, "Failed to send pkt to audio decoder!\n");
            goto __OUT;
        }

        while (ret >= 0){
            ret = avcodec_receive_frame(scp->audio_ctx, &scp->audio_frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF){
                break;
            }
            else if (ret < 0){
                av_log(scp->audio_ctx, AV_LOG_ERROR, "Failed to receive frame from audio decoder!\n");
                goto __OUT;
            }

            if (!scp->audio_swr_ctx){
                init_sws_audio(scp);
            }

            if (scp->audio_swr_ctx){
                const uint8_t **in = (const uint8_t **)scp->audio_frame.extended_data;
                int in_count = scp->audio_frame.nb_samples;
                uint8_t **out = &scp->audio_buf;
                int out_count = scp->audio_frame.nb_samples + 512;
                int out_size = av_samples_get_buffer_size(NULL,
                                                          scp->audio_frame.ch_layout.nb_channels,
                                                          out_count, AV_SAMPLE_FMT_S16, 0);
                unsigned int  malloc_size = 0;
                av_fast_malloc(&scp->audio_buf, &malloc_size, out_size);

                len2 = swr_convert(scp->audio_swr_ctx,
                                   out,
                                   out_count,
                                   in,
                                   in_count);

                data_size = len2 * scp->audio_frame.ch_layout.nb_channels * av_get_bytes_per_sample(AV_SAMPLE_FMT_S16);
            }else{
                scp->audio_buf = scp->audio_frame.data[0];
                data_size = av_samples_get_buffer_size(NULL,
                                                       scp->audio_frame.ch_layout.nb_channels,
                                                       scp->audio_frame.nb_samples,
                                                       scp->audio_frame.format,
                                                       1);
            }
            /* 只记 write_pts；主时钟在 wrote 重置时再挂 */
            if (!isnan(scp->audio_frame.pts)) {
                scp->audio_write_pts = sc_ts_to_ms(scp->audio_frame.pts, scp->audio_st->time_base);
            } else {
                scp->audio_write_pts = NAN;
            }
            scp->audio_buf_size = (unsigned int)FFMAX(data_size, 0);
            scp->audio_buf_cursor = 0;

            av_packet_unref(&scp->audio_pkt);
            av_frame_unref(&scp->audio_frame);
            return data_size;
        }
    }
__OUT:
    return ret;
}

/* EXTERNAL：有积压则重采样快消耗（不丢包）；无包复用上一盒并推 pts */
static int audio_decode_frame_external(SCPlayer *scp)
{
    int ret = -1;
    int len2;
    int data_size = 0;
    int wanted_nb_samples;

    if (scp->audioq.nb_packets <= 0) {
        return audio_reuse_last_buf(scp);
    }

    for (;;){
        ret = packet_queue_get(&scp->audioq, &scp->audio_pkt, 0);
        if (ret <= 0){
            return audio_reuse_last_buf(scp);
        }

        ret = avcodec_send_packet(scp->audio_ctx, &scp->audio_pkt);
        if (ret < 0){
            av_log(scp->audio_ctx, AV_LOG_ERROR, "Failed to send pkt to audio decoder!\n");
            av_packet_unref(&scp->audio_pkt);
            goto __OUT;
        }

        while (ret >= 0){
            ret = avcodec_receive_frame(scp->audio_ctx, &scp->audio_frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF){
                break;
            }
            else if (ret < 0){
                av_log(scp->audio_ctx, AV_LOG_ERROR, "Failed to receive frame from audio decoder!\n");
                av_packet_unref(&scp->audio_pkt);
                goto __OUT;
            }

            if (!scp->audio_swr_ctx){
                init_sws_audio(scp);
            }

            /* get 后还有包 = 没消耗完 → 强制快播 */
            wanted_nb_samples = sc_synchronize_audio_to_ext(scp, scp->audio_frame.nb_samples);

            /* 先 swr 成 S16（不做 compensation，避免变调） */
            if (!scp->audio_swr_ctx){
                init_sws_audio(scp);
            }
            if (scp->audio_swr_ctx){
                const uint8_t **in = (const uint8_t **)scp->audio_frame.extended_data;
                int in_count = scp->audio_frame.nb_samples;
                uint8_t **out = &scp->audio_buf;
                int out_count = scp->audio_frame.nb_samples + 256;
                int out_size = av_samples_get_buffer_size(NULL,
                                                          scp->audio_frame.ch_layout.nb_channels,
                                                          out_count, AV_SAMPLE_FMT_S16, 0);
                unsigned int  malloc_size = 0;
                av_fast_malloc(&scp->audio_buf, &malloc_size, out_size);
                swr_set_compensation(scp->audio_swr_ctx, 0, 0);
                len2 = swr_convert(scp->audio_swr_ctx, out, out_count, in, in_count);
                data_size = len2 * scp->audio_frame.ch_layout.nb_channels *
                            av_get_bytes_per_sample(AV_SAMPLE_FMT_S16);
            } else {
                scp->audio_buf = scp->audio_frame.data[0];
                data_size = av_samples_get_buffer_size(NULL,
                                                       scp->audio_frame.ch_layout.nb_channels,
                                                       scp->audio_frame.nb_samples,
                                                       scp->audio_frame.format,
                                                       1);
            }

            /* 需要加快消耗：atempo 变速不变调 */
            if (wanted_nb_samples > 0 &&
                wanted_nb_samples != scp->audio_frame.nb_samples &&
                data_size > 0 && scp->audio_buf) {
                double tempo = (double)scp->audio_frame.nb_samples / (double)wanted_nb_samples;
                int ch = scp->audio_frame.ch_layout.nb_channels;
                int rate = scp->audio_ctx->sample_rate;
                int at_ret = sc_atempo_process_s16(scp, scp->audio_buf, len2 > 0 ? len2 : scp->audio_frame.nb_samples,
                                                   ch, rate, tempo, &scp->audio_buf);
                if (at_ret > 0) {
                    data_size = at_ret;
                }
                /* atempo 失败则保持原速 S16，不变调硬拉 */
            }

            if (!isnan(scp->audio_frame.pts)) {
                scp->audio_write_pts = sc_ts_to_ms(scp->audio_frame.pts, scp->audio_st->time_base);
            } else {
                scp->audio_write_pts = NAN;
            }
            scp->audio_buf_size = (unsigned int)FFMAX(data_size, 0);
            scp->audio_buf_cursor = 0;

            av_packet_unref(&scp->audio_pkt);
            av_frame_unref(&scp->audio_frame);
            return data_size;
        }
        av_packet_unref(&scp->audio_pkt);
    }
__OUT:
    return ret;
}

void audio_decode_callback(void *userdata, uint8_t *stream, int len){
    SCPlayer *scp = (SCPlayer *)userdata;
    (void)stream;
    (void)len;
    if(scp->av_sync_type == AV_SYNC_AUDIO_MASTER){
        scp->out_audio_size = audio_decode_frame_audio(scp);
    }  else if(scp->av_sync_type == AV_SYNC_EXTERNAL_MASTER){
        scp->out_audio_size = audio_decode_frame_external(scp);
    }
   
}

/* flag 2：重置线性时钟基点 → get_audio_clock = pts + 线性(0~size) */
void audio_queue_wrote(SCPlayer *scp, int bytes)
{
    if (!scp || bytes <= 0) {
        return;
    }
    scp->audio_buf_cursor += (unsigned int)bytes;

    pthread_mutex_lock(&scp->audio_clock_mutex);
    if (!isnan(scp->audio_write_pts)) {
        scp->audio_clock = scp->audio_write_pts;
    }
    scp->audio_lin_bytes = (unsigned int)bytes;
    scp->audio_lin_wall_ms = av_gettime_ms();
    scp->audio_lin_elapsed_ms = 0;
    scp->audio_lin_ready = 1;
    pthread_mutex_unlock(&scp->audio_clock_mutex);

    /* 外部钟只钉一次；之后不被音频拉回 */
    if (scp->av_sync_type == AV_SYNC_EXTERNAL_MASTER) {
        sc_ext_clock_sync_to_slave(scp, get_audio_clock(scp));
    }
}


int audio_wanted_spec(void *opaque,
                      AVChannelLayout *wanted_channel_layout,
                      int wented_salple_rate){

    AudioInfo wanted_spec;
    int wanted_nb_channels = wanted_channel_layout->nb_channels;
    wanted_spec.freq = wented_salple_rate;
    wanted_spec.format = AV_SAMPLE_FMT_S16;
    wanted_spec.channels = wanted_nb_channels;
    wanted_spec.silence = 0;
    wanted_spec.samples = AUDIO_BUFFER_SIZE;
    wanted_spec.userdata = (void *)opaque;

    av_log(NULL,AV_LOG_INFO,
           "wanted spec: channels:%d,sample_fmt:%d,sanple_ret:%d \n",
           wanted_nb_channels,AV_SAMPLE_FMT_S16,wented_salple_rate);

    SCPlayer *scp = (SCPlayer*)opaque;
    scp->audioInfo = wanted_spec;
    return 0;
}

/*
 get_audio_clock = pts + 线性时钟
 线性时钟：自上次 wrote 重置起的墙钟，钳在本盒 0~size 对应时长
 */
double get_audio_clock(SCPlayer *scp){
    double pts;
    double elapsed;

    if (!scp) {
        return 0;
    }

    pthread_mutex_lock(&scp->audio_clock_mutex);
    pts = scp->audio_clock;
    elapsed = audio_lin_elapsed_ms_l(scp);
    pthread_mutex_unlock(&scp->audio_clock_mutex);

    if (isnan(pts)) {
        return 0;
    }
    return pts + elapsed;
}
