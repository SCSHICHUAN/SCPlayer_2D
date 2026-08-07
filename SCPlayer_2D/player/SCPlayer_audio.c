//
//  SCPlayer_audio.c
//  SCPlayer_2D
//
//  Created by Stan on 2026/8/3.
//  Copyright © 2026 石川. All rights reserved.
//

#include "SCPlayer_audio.h"
#include <math.h>

//音频重采样初始化
void init_sws_audio(SCPlayer *scp){
    AVChannelLayout in_ch_layout, out_ch_layout;
    av_channel_layout_copy(&in_ch_layout, &scp->audio_ctx->ch_layout);
    av_channel_layout_copy(&out_ch_layout, &in_ch_layout);
    if (scp->audio_ctx->sample_fmt != AV_SAMPLE_FMT_S16)
    { // 需要重采样
        swr_alloc_set_opts2(&scp->audio_swr_ctx,
                            &out_ch_layout,        // 输出声道数
                            AV_SAMPLE_FMT_S16,     // 输出采样深度
                            scp->audio_ctx->sample_rate, // 输出采样率
                            &in_ch_layout,         // 输入省道数
                            scp->audio_ctx->sample_fmt,  // 输入采样深度
                            scp->audio_ctx->sample_rate, // 输入采样率
                            0,
                            NULL);
        swr_init(scp->audio_swr_ctx);
    }
}

//=========================== 解码音频包保存到音频帧到队列中 ===========================
static int audio_decode_frame(SCPlayer *scp)
{
    int ret = -1;
    int len2;
    int data_size = 0;
    // AVPacket pkt;

    for (;;){
         //从队列中读取数据
        if (packet_queue_get(&scp->audioq, &scp->audio_pkt, 1) < 0){ // 队列中读取数据
            av_log(NULL,AV_LOG_ERROR,"不能从音频包queue中读取取包!\n");
            scp->quit = 1;
            break;
        }

        //发送音频pkt解码
        ret = avcodec_send_packet(scp->audio_ctx, &scp->audio_pkt);
        if (ret < 0){
            av_log(scp->audio_ctx, AV_LOG_ERROR, "Failed to send pkt to audio decoder!\n");
            goto __OUT;
        }

        /**
         鉴于解码器是异步的处理，通常解码线程处理中 avcodec_send_packet() 和 avcodec_receive_frame()
         也不是一对一的使用的，为了确保没有遗漏的解码帧，可以调用1次送入包，反复调用解码直到没有帧输出
         */
        while (ret >= 0){//轮询解码包

            ret = avcodec_receive_frame(scp->audio_ctx, &scp->audio_frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF){
                break; // 如果是这两种错误,退出到上层循环 继续读取数据
            }
            else if (ret < 0){
                // 解码失败退出
                av_log(scp->audio_ctx, AV_LOG_ERROR, "Failed to receive frame from audio decoder!\n");
                goto __OUT;
            }

            /*
             音频重采样 统一到 AV_SAMPLE_FMT_S16
             */
            // 初始化音频重采样上下文
            if (!scp->audio_swr_ctx){
                init_sws_audio(scp);
            }

            if (scp->audio_swr_ctx){
                // 输入 拿到原始的数据 在音频AVFrame中 extended_data域 = data域
                const uint8_t **in = (const uint8_t **)scp->audio_frame.extended_data;
                int in_count = scp->audio_frame.nb_samples; // 采样个数
                // 输出
                uint8_t **out = &scp->audio_buf;               // 重采样输出数据
                int out_count = scp->audio_frame.nb_samples + 512; // 单声道的一次的采样个数,输出采样个数 512 融于值
                // 一个包字节(B) = 声道数 × out_count(一个声道从采样个数) × 2 (AV_SAMPLE_FMT_S16 = 2B)
                int out_size = av_samples_get_buffer_size(NULL,
                                                          scp->audio_frame.ch_layout.nb_channels,
                                                          out_count, AV_SAMPLE_FMT_S16, 0);
                // 输出的音频包开辟空间 实际开辟的空间audio_buf_size，out_size想要分配空间的大小
                unsigned int  malloc_size = 0;
                av_fast_malloc(&scp->audio_buf, &malloc_size, out_size);
                
                // 音频重采样,返回一个声道的实际转换出的采样个数
                len2 = swr_convert(scp->audio_swr_ctx,
                                   out,
                                   out_count,
                                   in,
                                   in_count);

                data_size = len2 * scp->audio_frame.ch_layout.nb_channels * av_get_bytes_per_sample(AV_SAMPLE_FMT_S16);
                // data_size = （len2）采样个数 x （nb_channels）音频通道数 x 位深
//                printf("len2=%d, nb_channels=%d, bytes_per_sample=%d\n",
//                       len2,scp->audio_frame.ch_layout.nb_channels,
//                       av_get_bytes_per_sample(AV_SAMPLE_FMT_S16));
            }else{
                // 不需要采样,直接拷贝
                scp->audio_buf = scp->audio_frame.data[0];
                data_size = av_samples_get_buffer_size(NULL,
                                                       scp->audio_frame.ch_layout.nb_channels,
                                                       scp->audio_frame.nb_samples,
                                                       scp->audio_frame.format,
                                                       1);
            }          
            // audio_clock = 本帧 pts 起点（ms）；播出时刻再靠 cursor / aq 水位修正
            if(!isnan(scp->audio_frame.pts)){
                scp->audio_clock = sc_ts_to_ms(scp->audio_frame.pts, scp->audio_st->time_base);
            }else{
                scp->audio_clock = NAN;
            }
            scp->audio_buf_size = (unsigned int)FFMAX(data_size, 0);
            scp->audio_buf_cursor = 0; /* 尚未交给 AudioQueue */

            //清空
            av_packet_unref(&scp->audio_pkt);
            av_frame_unref(&scp->audio_frame);
            return data_size;
        }
    }
__OUT:
    return ret;
}


/* 解码一帧 PCM 到 scp->audio_buf；enqueue 后由上层推进游标 / aq_size */
void audio_decode_callback(void *userdata, uint8_t *stream, int len)
{
    SCPlayer *scp = (SCPlayer *)userdata;
    (void)stream;
    (void)len;
    scp->out_audio_size = audio_decode_frame(scp);
}

/* Enqueue 成功：本帧 cursor 前进，硬件水位上升（PCM 已由 AQ 持有，软件侧可被下一帧覆盖） */
void audio_queue_wrote(SCPlayer *scp, int bytes)
{
    if (!scp || bytes <= 0) {
        return;
    }
    scp->audio_buf_cursor += (unsigned int)bytes;
    scp->audio_aq_size += (unsigned int)bytes;
}

/* 某 AQ buffer 播完回收：水位下降（不依赖仍存在的 PCM，只记字节账） */
void audio_queue_consumed(SCPlayer *scp, int bytes)
{
    if (!scp || bytes <= 0) {
        return;
    }
    if (scp->audio_aq_size >= (unsigned int)bytes) {
        scp->audio_aq_size -= (unsigned int)bytes;
    } else {
        scp->audio_aq_size = 0;
    }
}

//希望到音频目标参数
int audio_wanted_spec(void *opaque,
                      AVChannelLayout *wanted_channel_layout,
                      int wented_salple_rate){

    /*SDL_OpenAudio需要传入两个参数，一个是我们想要的音频格式。一个是最后实际的音频格式。
     这里的SDL_AudioSpec，是SDL中记录音频格式的结构体。
     &spec 告诉调用者实际的参数
     */
    AudioInfo wanted_spec;
    int wanted_nb_channels = wanted_channel_layout->nb_channels;
    /*9.初始化音频设备参数
      为音频设备设置参数
    */
    wanted_spec.freq = wented_salple_rate; // 采样率
    wanted_spec.format = AV_SAMPLE_FMT_S16; // 有符号的16位
    wanted_spec.channels = wanted_nb_channels;
    wanted_spec.silence = 0;                 // 静默音
    wanted_spec.samples = AUDIO_BUFFER_SIZE; // 采样个数
//    wanted_spec.callback = sdl_audio_callback;
    wanted_spec.userdata = (void *)opaque;

    av_log(NULL,AV_LOG_INFO,
           "wanted spec: channels:%d,sample_fmt:%d,sanple_ret:%d \n",
           wanted_nb_channels,AV_SAMPLE_FMT_S16,wented_salple_rate);

//    if (SDL_OpenAudio(&wanted_spec, &spec) < 0){
//        av_log(NULL, AV_LOG_ERROR, "打开音频设备失败!\n");
//        return -1;
//    }
//    return spec.size;
    SCPlayer *scp = (SCPlayer*)opaque;
    scp->audioInfo = wanted_spec;
    return 0;
}
//每秒字节数 = 采样率 × 声道数 × 每次采样字节数(16为的是 2) = sample_rate × channels × 2
static int audio_bytes_per_sec(SCPlayer *scp)
{
    if (!scp->audio_ctx) {
        return 0;
    }
    /* 输出 PCM 为 S16 interleaved, sample_rate:samples per second */
    return scp->audio_ctx->sample_rate * scp->audio_ctx->ch_layout.nb_channels * 2;
}

/*
 当前播放时刻（ms）≈ 本帧起点 pts + 已交给设备的时长 − AQ 未播完时长
 aq 水位不存 PCM，只记账：解码会覆盖 audio_buf，硬件延迟靠字节账
 */
double get_audio_clock(SCPlayer *scp){
    double pts;
    int bytes_per_sec;

    pts = scp->audio_clock;
    if (isnan(pts)) {
        return 0;
    }

    bytes_per_sec = audio_bytes_per_sec(scp);
    if (!bytes_per_sec) {
        return pts;
    }
   
    //包的开始时间 + 已经写入的设备数据的时间(写不代表已经播放)
    pts += sc_sec_to_ms((double)scp->audio_buf_cursor / bytes_per_sec);
    if (scp->audio_aq_size > 0) {
        //设备真播放调的数据的时间
        pts -= sc_sec_to_ms((double)scp->audio_aq_size / bytes_per_sec);
    }
    return pts;
}
