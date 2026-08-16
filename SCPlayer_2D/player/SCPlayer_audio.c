//
//  SCPlayer_audio.c
//  SCPlayer_2D
//
//  Created by Stan on 2026/8/3.
//  Copyright © 2026 石川. All rights reserved.
//

#include "SCPlayer_audio.h"
#include "SCPlayer_sync.h"
#include "SCPlayer_PLC.h"
#include <math.h>
#include <string.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libavutil/opt.h>

#define SC_SAMPLE_CORRECTION_PERCENT_MAX 10

static void atempo_close(SCPlayer *scp);
double get_audio_pkt_elapsed_ms(SCPlayer *scp);
int audio_reuse_last_buf(SCPlayer *scp);
int audio_bytes_per_sec(SCPlayer *scp);

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

void audio_aq_interp_start(SCPlayer *scp)
{
    if (!scp) {
        return;
    }
    pthread_mutex_init(&scp->audio_clock_mutex, NULL);
    scp->audio_clock = NAN;
    scp->audio_frame_pts = NAN;
    scp->audio_compensation_pts = 0;
    scp->audio_pkt_wall_ms = 0;
    scp->audio_pkt_bytes = 0;
    scp->audio_pkt_ready = 0;
    scp->audio_pkt_paused = 0;
    scp->audio_pkt_elapsed_ms = 0;
    /* 补帧暖机计时在首次 wrote/送显时再钉；此处清掉 */
    scp->compensate_warm_start_ms = NAN;
    /* PLC 历史栈仅低延时 */
    if (sc_av_compensate_enabled(scp)) {
        if (!scp->audio_plc) {
            scp->audio_plc = sc_audio_plc_create();
        } else {
            sc_audio_plc_reset(scp->audio_plc);
        }
    } else {
        sc_audio_plc_destroy(scp->audio_plc);
        scp->audio_plc = NULL;
        scp->audio_compensation_pts = 0;
    }
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
    atempo_close(scp);
    sc_audio_plc_destroy(scp->audio_plc);
    scp->audio_plc = NULL;
    scp->audio_compensation_pts = 0;
    pthread_mutex_destroy(&scp->audio_clock_mutex);
    scp->audio_pkt_ready = 0;
}

void audio_aq_set_paused(SCPlayer *scp, int paused)
{
    if (!scp) {
        return;
    }
    pthread_mutex_lock(&scp->audio_clock_mutex);
    if (paused && !scp->audio_pkt_paused) {
        scp->audio_pkt_elapsed_ms = get_audio_pkt_elapsed_ms(scp);
        scp->audio_pkt_paused = 1;
    } else if (!paused && scp->audio_pkt_paused) {
        /* 恢复：把墙钟起点挪到「等效已流逝」处 */
        scp->audio_pkt_wall_ms = av_gettime_ms() - scp->audio_pkt_elapsed_ms;
        scp->audio_pkt_paused = 0;
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

static void atempo_close(SCPlayer *scp)
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
static int atempo_open(SCPlayer *scp, int sample_rate, AVChannelLayout *ch_layout, double tempo)
{
    char args[256];
    char ch_desc[128];
    int ret;
    const AVFilter *abuffersrc;
    const AVFilter *abuffersink;
    const AVFilter *atempo;
    AVFilterContext *atempo_ctx = NULL;
    AVFilterGraph *graph;

    atempo_close(scp);
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
static int atempo_process_s16(SCPlayer *scp, uint8_t *pcm, int nb_samples,
                                 int channels, int sample_rate, double tempo,
                                 uint8_t **out_pcm)
{
    AVFrame *in_frame = NULL;
    AVFrame *out_frame = NULL;
    int ret;
    int out_bytes = 0;
    AVChannelLayout ch = {0};

    if (!scp || !pcm || nb_samples <= 0) {
        return -1;
    }
    if (fabs(tempo - 1.0) < 0.001) {
        return nb_samples * channels * 2;
    }

    av_channel_layout_default(&ch, channels);
    if (!scp->audio_filter_graph || fabs(scp->audio_filter_tempo - tempo) > 0.001) {
        ret = atempo_open(scp, sample_rate, &ch, tempo);
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

    /* 主路径：推进滤镜图 → atempo 压短 → 取出更短 PCM */
    ret = av_buffersrc_add_frame_flags(scp->audio_buffersrc_ctx, in_frame, 0);
    if (ret < 0) {
        goto end;
    }

    ret = av_buffersink_get_frame(scp->audio_buffersink_ctx, out_frame);
    if (ret < 0) {
        goto end;
    }

    out_bytes = out_frame->nb_samples * channels * 2; /* 输出采样变少 → 墙钟更短 */
    av_fast_malloc(out_pcm, &scp->audio_buf_alloc, out_bytes + 32);
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
static int synchronize_audio_to_external(SCPlayer *scp, int nb_samples)
{
    int wanted;
    double diff;
    double external;
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
    
    /*
     回置区间 [0.01 0.03] 不在灵界点来哈却换导致音频声音奇怪
     如果队列小于0.01MB时就正常播放，一直保持这个速度让队列增长，
     增长到0.03MB就开始加速让队列减少，一直保持这个速度让队列减少，
     减少到0.01MB切回正常速度,
     这里还可以优化,观察是否增长和减少
     */
    double audio_q_size = scp->audioq.size / (1024.0 * 1024.0);
    if (audio_q_size <= 0.01) {// 小于0.01MB 就按照 正常的数度一直播
        scp->hysteresis_samples = nb_samples; //正常播放 慢数度
    } else if (audio_q_size >= 0.05){
        scp->hysteresis_samples = nb_samples * (100 - 50) / 100; //加快50%,最大只能是2倍
    } else if (audio_q_size >= 0.03) {// 小于0.03MB 就按照就一直加速播放
        scp->hysteresis_samples = nb_samples * (100 - 10) / 100;//加快10%
    }
    
    if (scp->hysteresis_samples < 1) {
        scp->hysteresis_samples = 1;
    }

    
    /* 队列没消耗干净：直接最大加速 */
    wanted = scp->hysteresis_samples;

    external = get_external_clock(scp);
    if (!isnan(external)) {
        diff = get_audio_clock(scp) - external;
        if (!isnan(diff) && diff < 0 &&
            fabs(diff) >= scp->audio_diff_threshold_ms &&
            fabs(diff) < SC_NOSYNC_THRESHOLD_MS) {
            int by_diff = nb_samples + (int)(diff / SC_S_TO_MS * (double)freq);
            if (by_diff < wanted) {
                wanted = by_diff;
            }
            if (wanted < scp->hysteresis_samples) {
                wanted = scp->hysteresis_samples;
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


//=========================== 解码音频包保存到音频帧到队列中 ===========================
/* 高延时(AUDIO_MASTER)：经典解码；无包返回 0，由上层填静音；不做补偿 */
int audio_decode_frame_audio(SCPlayer *scp)
{
    int ret = -1;
    int len2;
    int data_size = 0;

    for (;;){
        ret = packet_queue_get(&scp->audioq, &scp->audio_pkt, 0);
        if (ret <= 0){
            return 0;
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

            if (scp->audio_swr_ctx){
                const uint8_t **in = (const uint8_t **)scp->audio_frame.extended_data;
                int in_count = scp->audio_frame.nb_samples;
                uint8_t **out = &scp->audio_buf;
                int out_count = scp->audio_frame.nb_samples + 512;
                int out_size = av_samples_get_buffer_size(NULL,
                                                          scp->audio_frame.ch_layout.nb_channels,
                                                          out_count, AV_SAMPLE_FMT_S16, 0);
                av_fast_malloc(&scp->audio_buf, &scp->audio_buf_alloc, out_size);

                len2 = swr_convert(scp->audio_swr_ctx,
                                   out,
                                   out_count,
                                   in,
                                   in_count);

                data_size = len2 * scp->audio_frame.ch_layout.nb_channels * av_get_bytes_per_sample(AV_SAMPLE_FMT_S16);
            }else{
                data_size = av_samples_get_buffer_size(NULL,
                                                       scp->audio_frame.ch_layout.nb_channels,
                                                       scp->audio_frame.nb_samples,
                                                       scp->audio_frame.format,
                                                       1);
                av_fast_malloc(&scp->audio_buf, &scp->audio_buf_alloc, (unsigned int)FFMAX(data_size, 0));
                if (scp->audio_buf && data_size > 0 && scp->audio_frame.data[0]) {
                    memcpy(scp->audio_buf, scp->audio_frame.data[0], (size_t)data_size);
                }
            }
            if (!isnan(scp->audio_frame.pts)) {
                scp->audio_frame_pts = sc_ts_to_ms(scp->audio_frame.pts, scp->audio_st->time_base);
            } else {
                scp->audio_frame_pts = NAN;
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


/* 低延时无包：历史栈反向补偿；推进 frame_pts 并累计 compensation */
int audio_reuse_last_buf(SCPlayer *scp)
{
    int bps;
    int ch;
    int rate;
    int nb;
    int bytes;
    double dur_ms;

    if (!scp || !scp->audio_ctx || !sc_av_compensate_enabled(scp)) {
        return 0;
    }
    /*
     开播暖机：未 wrote / 无 PLC 历史 / 未满 5s → 不补、不加 compensation。
     返回 0 → 上层填静音且不走 wrote。
     */
    if (!scp->audio_pkt_ready || sc_audio_plc_count(scp->audio_plc) <= 0 ||
        !sc_compensate_warm_done(scp)) {
        return 0;
    }
    ch = scp->audio_ctx->ch_layout.nb_channels;
    rate = scp->audio_ctx->sample_rate;
    if (ch <= 0 || rate <= 0) {
        return 0;
    }
    nb = sc_audio_plc_last_nb_samples(scp->audio_plc);
    if (nb <= 0) {
        nb = AUDIO_BUFFER_SIZE;
    }
    bytes = nb * ch * (int)sizeof(int16_t);
    av_fast_malloc(&scp->audio_buf, &scp->audio_buf_alloc, (unsigned int)bytes);
    if (!scp->audio_buf) {
        return 0;
    }

    if (scp->audio_plc) {
        sc_audio_plc_fill(scp->audio_plc, (int16_t *)scp->audio_buf, nb, ch, rate);
    } else {
        memset(scp->audio_buf, 0, (size_t)bytes);
    }

    bps = audio_bytes_per_sec(scp);
    dur_ms = (bps > 0) ? sc_sec_to_ms((double)bytes / (double)bps) : 0;
    if (dur_ms > 0) {
        /* 例如补了 10ms：之后所有真实包 pts 都 +10ms */
        scp->audio_compensation_pts += dur_ms;
        if (!isnan(scp->audio_frame_pts)) {
            scp->audio_frame_pts += dur_ms;
        } else {
            scp->audio_frame_pts = scp->audio_compensation_pts;
        }
    }
    scp->audio_buf_size = (unsigned int)bytes;
    scp->audio_buf_cursor = 0;
    return bytes;
}

/* 低延时(EXTERNAL)：有积压则 atempo 快消耗；无包走历史栈补偿 */
int audio_decode_frame_external(SCPlayer *scp)
{
    int ret = -1;
    int len2 = 0;
    int data_size = 0;
    int wanted_nb_samples;

    if (!sc_av_compensate_enabled(scp)) {
        /* 不应走到；兜底走高延时解码 */
        return audio_decode_frame_audio(scp);
    }

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
                av_fast_malloc(&scp->audio_buf, &scp->audio_buf_alloc, out_size);
                swr_set_compensation(scp->audio_swr_ctx, 0, 0);
                len2 = swr_convert(scp->audio_swr_ctx, out, out_count, in, in_count);
                data_size = len2 * scp->audio_frame.ch_layout.nb_channels *
                            av_get_bytes_per_sample(AV_SAMPLE_FMT_S16);
            } else {
                data_size = av_samples_get_buffer_size(NULL,
                                                       scp->audio_frame.ch_layout.nb_channels,
                                                       scp->audio_frame.nb_samples,
                                                       scp->audio_frame.format,
                                                       1);
                av_fast_malloc(&scp->audio_buf, &scp->audio_buf_alloc, (unsigned int)FFMAX(data_size, 0));
                if (scp->audio_buf && data_size > 0 && scp->audio_frame.data[0]) {
                    memcpy(scp->audio_buf, scp->audio_frame.data[0], (size_t)data_size);
                }
            }
            
            /* get 后还有包 = 没消耗完 → 强制快播 */
            wanted_nb_samples = synchronize_audio_to_external(scp, scp->audio_frame.nb_samples);

            /* 需要加快消耗：atempo 变速不变调 */
            if (wanted_nb_samples > 0 &&
                wanted_nb_samples != scp->audio_frame.nb_samples &&
                data_size > 0 && scp->audio_buf) {
                double tempo = (double)scp->audio_frame.nb_samples / (double)wanted_nb_samples;
                int ch = scp->audio_frame.ch_layout.nb_channels;
                int rate = scp->audio_ctx->sample_rate;
                int at_ret = atempo_process_s16(scp, scp->audio_buf, len2 > 0 ? len2 : scp->audio_frame.nb_samples,
                                                   ch, rate, tempo, &scp->audio_buf);
                if (at_ret > 0) {
                    data_size = at_ret;
                    /* atempo 可能 realloc 了 audio_buf，容量未知：按输出字节记下限 */
                    if (scp->audio_buf_alloc < (unsigned int)data_size) {
                        scp->audio_buf_alloc = (unsigned int)data_size;
                    }
                }
                /* atempo 失败则保持原速 S16，不变调硬拉 */
            }

            
            if (!isnan(scp->audio_frame.pts)) {
                scp->audio_frame_pts = sc_ts_to_ms(scp->audio_frame.pts, scp->audio_st->time_base)
                                       + scp->audio_compensation_pts;
            } else {
                scp->audio_frame_pts = NAN;
            }
            scp->audio_buf_size = (unsigned int)FFMAX(data_size, 0);
            scp->audio_buf_cursor = 0;

            /* 压入历史栈，供无包时反向补偿 */
            if (scp->audio_plc && scp->audio_buf && data_size > 0 && scp->audio_ctx) {
                int ch = scp->audio_frame.ch_layout.nb_channels;
                int rate = scp->audio_ctx->sample_rate;
                int nb = (ch > 0) ? (data_size / (ch * (int)sizeof(int16_t))) : 0;
                if (nb > 0 && rate > 0) {
                    sc_audio_plc_rx(scp->audio_plc, (const int16_t *)scp->audio_buf, nb, ch, rate);
                }
            }

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
    /* 以 latency_mode 为准，避免与 av_sync_type 短暂不一致走错路 */
    if (scp->latency_mode == SC_LATENCY_LOW) {
        scp->out_audio_size = audio_decode_frame_external(scp);
    } else {
        scp->out_audio_size = audio_decode_frame_audio(scp);
    }
}

//=====================================音频时钟线性差值计算===============================================
//  1s的时间播放掉多少音频数据 bytes
int audio_bytes_per_sec(SCPlayer *scp)
{
    if (!scp->audio_ctx) {
        return 0;
    }
    return scp->audio_ctx->sample_rate * scp->audio_ctx->ch_layout.nb_channels * 2;
}
// 一个音频包已播时掉的时间ms
double get_audio_pkt_elapsed_ms(SCPlayer *scp)
{
    int bps;
    double elapsed;
    double max_ms;

    if (!scp->audio_pkt_ready) {
        return 0;
    }
    if (scp->audio_pkt_paused) {
        return scp->audio_pkt_elapsed_ms;
    }

    elapsed = av_gettime_ms() - scp->audio_pkt_wall_ms;
    if (elapsed < 0) {
        elapsed = 0;
    }
    
    //判断是不是超过了这一个包的时间
    bps = audio_bytes_per_sec(scp);
    if (bps > 0 && scp->audio_pkt_bytes > 0) {
        max_ms = sc_sec_to_ms((double)scp->audio_pkt_bytes / (double)bps);
        if (elapsed > max_ms) {
            elapsed = max_ms;
        }
    }
    return elapsed;
}


/* 新包入队：重置本包时钟基点 → get_audio_clock = pts + 本包已播时长 */
void audio_queue_wrote(SCPlayer *scp, int bytes)
{
    if (!scp || bytes <= 0) {
        return;
    }
    scp->audio_buf_cursor += (unsigned int)bytes;

    pthread_mutex_lock(&scp->audio_clock_mutex);
    if (!isnan(scp->audio_frame_pts)) {
        scp->audio_clock = scp->audio_frame_pts;
    }
    scp->audio_pkt_bytes = (unsigned int)bytes;
    
    scp->audio_pkt_wall_ms = av_gettime_ms();
   
    scp->audio_pkt_elapsed_ms = 0;
    scp->audio_pkt_ready = 1;
    pthread_mutex_unlock(&scp->audio_clock_mutex);

    if (sc_av_compensate_enabled(scp)) {
        sc_compensate_warm_mark_playing(scp);
        /* 外部钟只钉一次；高延时不碰 external */
        external_clock_sync_to_slave(scp, get_audio_clock(scp));
    }
}

/*
 get_audio_clock = pts + 本包已播时长
 本包：自上次 wrote 起的墙钟推进，钳在 0~本包字节对应时长
 */
double get_audio_clock(SCPlayer *scp){
    double pts;
    double elapsed;

    if (!scp) {
        return 0;
    }

    pthread_mutex_lock(&scp->audio_clock_mutex);
    pts = scp->audio_clock;
    elapsed = get_audio_pkt_elapsed_ms(scp);
    pthread_mutex_unlock(&scp->audio_clock_mutex);

    if (isnan(pts)) {
        return 0;
    }
    return pts + elapsed;
}
