//
//  SCPlayer_audio.c
//  SCPlayer_2D
//
//  Created by Stan on 2026/8/3.
//  Copyright © 2026 石川. All rights reserved.
//

#include "SCPlayer_audio.h"
#include <math.h>

//=========================== 解码音频包保存到音频帧到队列中 ===========================
static int audio_decode_frame(VideoState *is)
{
    int ret = -1;
    int len2;
    int data_size = 0;
    // AVPacket pkt;

    for (;;){
         //从队列中读取数据
        if (packet_queue_get(&is->audioq, &is->audio_pkt, 1) < 0){ // 队列中读取数据
            av_log(NULL,AV_LOG_ERROR,"不能从音频包queue中读取取包!\n");
            is->quit = 1;
            break;
        }

        ret = avcodec_send_packet(is->audio_ctx, &is->audio_pkt);
        if (ret < 0){
            av_log(is->audio_ctx, AV_LOG_ERROR, "Failed to send pkt to audio decoder!\n");
            goto __OUT;
        }

        /**
         鉴于解码器是异步的处理，通常解码线程处理中 avcodec_send_packet() 和 avcodec_receive_frame()
         也不是一对一的使用的，为了确保没有遗漏的解码帧，可以调用1次送入包，反复调用解码直到没有帧输出
         */
        while (ret >= 0){

            ret = avcodec_receive_frame(is->audio_ctx, &is->audio_frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF){
                break; // 如果是这两种错误退出到上层循环 继续读取数据
            }
            else if (ret < 0){
                // 解码失败退出
                av_log(is->audio_ctx, AV_LOG_ERROR, "Failed to receive frame from audio decoder!\n");
                goto __OUT;
            }

            /*
             音频重采样 统一到 AV_SAMPLE_FMT_S16
             */
            // 初始化音频重采样上下文
            if (!is->audio_swr_ctx){
                AVChannelLayout in_ch_layout, out_ch_layout;
                av_channel_layout_copy(&in_ch_layout, &is->audio_ctx->ch_layout);
                av_channel_layout_copy(&out_ch_layout, &in_ch_layout);
                if (is->audio_ctx->sample_fmt != AV_SAMPLE_FMT_S16)
                { // 需要重采样
                    swr_alloc_set_opts2(&is->audio_swr_ctx,
                                        &out_ch_layout,        // 输出声道数
                                        AV_SAMPLE_FMT_S16,     // 输出采样深度
                                        is->audio_ctx->sample_rate, // 输出采样率
                                        &in_ch_layout,         // 输入省道数
                                        is->audio_ctx->sample_fmt,  // 输入采样深度
                                        is->audio_ctx->sample_rate, // 输入采样率
                                        0,
                                        NULL);
                    swr_init(is->audio_swr_ctx);
                }
            }

            if (is->audio_swr_ctx){
                // 输入 拿到原始的数据 在音频AVFrame中 extended_data域 = data域
                const uint8_t **in = (const uint8_t **)is->audio_frame.extended_data;
                int in_count = is->audio_frame.nb_samples; // 采样个数
                // 输出
                uint8_t **out = &is->audio_buf;               // 重采样输出数据
                int out_count = is->audio_frame.nb_samples + 512; // 输出采样个数 512 融于值
                int out_size = av_samples_get_buffer_size(NULL,
                                                          is->audio_frame.ch_layout.nb_channels,
                                                          out_count, AV_SAMPLE_FMT_S16, 0);
                // 输出的音频包开辟空间 实际开辟的空间audio_buf_size，out_size想要分配空间的大小
                unsigned int  malloc_size = 0;
                av_fast_malloc(&is->audio_buf, &malloc_size, out_size);
                // 音频重采样
                len2 = swr_convert(is->audio_swr_ctx,
                                   out,
                                   out_count,
                                   in,
                                   in_count);
                // data_size = （len2）采样个数 x （nb_channels）音频通道数 x 位深
                printf("len2=%d, nb_channels=%d, bytes_per_sample=%d\n",len2,is->audio_frame.ch_layout.nb_channels,
                       av_get_bytes_per_sample(AV_SAMPLE_FMT_S16));
                data_size = len2 * is->audio_frame.ch_layout.nb_channels * av_get_bytes_per_sample(AV_SAMPLE_FMT_S16);
            }else{
                // 不需要采样
                is->audio_buf = is->audio_frame.data[0];
                data_size = av_samples_get_buffer_size(NULL,
                                                       is->audio_frame.ch_layout.nb_channels,
                                                       is->audio_frame.nb_samples,
                                                       is->audio_frame.format,
                                                       1);
            }          
            //计算音频已经播放的时间
            if(!isnan(is->audio_frame.pts)){
                /*
                is->audio_clock = pts + 帧的持续时间 （先加进来在减）
                当前播放调时刻  =  is->audio_clock - 音频帧剩余数据/bytes_per_sec
                 */
                is->audio_clock = is->audio_frame.pts * av_q2d(is->audio_st->time_base)
                                  + is->audio_frame.nb_samples / is->audio_frame.sample_rate;
            }else{
                is->audio_clock = NAN;
            }
           
            //清空
            av_packet_unref(&is->audio_pkt);
            av_frame_unref(&is->audio_frame);
            return data_size;
        }
    }
__OUT:
    return ret;
}

/*
  len 声卡需要的音频长度
  实际可能不能给len
 */
void sdl_audio_callback_1(void *userdata, uint8_t *stream, int len)
{
    VideoState *is = (VideoState *)userdata;
    is->out_audio_size = audio_decode_frame(is);
    
    //每次声卡要的数据
    len = is->out_audio_size;
    is->audio_buf_index = is->out_audio_size;
}




int audio_open(void *opaque,
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
    wanted_spec.format = AUDIO_S16SYS;    // 有符号的16位
    wanted_spec.channels = wanted_nb_channels;
    wanted_spec.silence = 0;                 // 静默音
    wanted_spec.samples = AUDIO_BUFFER_SIZE; // 采样个数
//    wanted_spec.callback = sdl_audio_callback;
    wanted_spec.userdata = (void *)opaque;

    av_log(NULL,AV_LOG_INFO,
           "wanted spec: channels:%d,sample_fmt:%d,sanple_ret:%d \n",
           wanted_nb_channels,AUDIO_S16,wented_salple_rate);

//    if (SDL_OpenAudio(&wanted_spec, &spec) < 0){
//        av_log(NULL, AV_LOG_ERROR, "打开音频设备失败!\n");
//        return -1;
//    }
//    return spec.size;
    VideoState *is = (VideoState*)opaque;
    is->audioInfo = wanted_spec;
    return 0;
}

//音频的clock
double get_audio_clock(VideoState *is){
    double pts;
    int hw_buf_size,bytes_per_sec,n;

    pts = is->audio_clock;/* maintained in the audio thread 在音频线程中维护*/
    hw_buf_size = is->audio_buf_size - is->audio_buf_index;// 当前缓冲区数据还剩余的
    bytes_per_sec = 0;
    n = is->audio_ctx->ch_layout.nb_channels * 2;//每个音频样本占用2个字节（通常是16位，等于2字节）。
    if(is->audio_st){
        bytes_per_sec = is->audio_ctx->sample_rate * n;//44100 * 4;采样率 x 字节数 = 每秒的字节数
    }
    if(bytes_per_sec){
        //当前剩余的pts
        pts -= (double)hw_buf_size / bytes_per_sec;//时间pts = 缓冲的bytes/(每秒多少bytes)
    }
    return pts;
}
