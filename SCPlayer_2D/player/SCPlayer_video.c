//
//  SCPlayer_video.c
//  SCPlayer_2D
//
//  Created by Stan on 2026/8/3.
//  Copyright © 2026 石川. All rights reserved.
//

#include "SCPlayer_video.h"
#include <math.h>
#include <libavutil/hwcontext.h>

//推算pts 因为有时会没有pts
static double synchronize_video(SCPlayer *scp,AVFrame *sec_frame,double pts){
    double frame_delay;
    
    if(pts != 0){
        scp->video_clock = pts;
    } else {
        //如果这一帧没有pts，说明这一帧播放的时间不确定，把之前保存的video_clock给pts
        pts = scp->video_clock;
    }
    /* time_base 单位为秒，产出转为 ms */
    frame_delay = sc_sec_to_ms(av_q2d(scp->video_ctx->time_base));
    /* if we are repeating a frame, adjust clock accordingly
     重复帧用于平滑视频播放，并调整帧率
     重复帧（repeat_pict）是一个表示当前帧需要重复显示的次数的值，一般为0不重复
     帧率平滑：通过使用半帧时间的增加量，视频解码器可以更准确地分配时间戳，
     以平滑帧率转换。例如，从 24 FPS 转换到 30 FPS，或其他帧率转换。
     */
    frame_delay += sec_frame->repeat_pict * (frame_delay * 0.5);
    
    scp->video_clock += frame_delay;
    return pts;
}
//保存到帧队列中
static int queue_pitcure(SCPlayer *scp,AVFrame *src_frame,
                         double pts,double duration,int64_t pos){
    Frame *vp;
    if(!(vp = frame_queue_peek_writable(&scp->pictq)))
        return -1;
    
    vp->sar = src_frame->sample_aspect_ratio;//SAR 表示单个像素的宽高比 SAR 为 1:1，则为方形像素
    
    vp->width = src_frame->width;   //帧的宽
    vp->height = src_frame->height; //帧的高
    vp->format = src_frame->format; //像素格式 YUV420P、RGB24
    
    vp->pts = pts;
    vp->duration = duration;
    vp->pos = pos;
    
    /*
     void av_frame_move_ref(AVFrame *dst, AVFrame *src)
     是 FFmpeg 库中用于在两个 AVFrame 结构体之间移动引用数据的函数。
     这意味着将源帧 (src) 的数据和引用移动到目标帧 (dst)，同时清空源帧的数据。
     这在处理视频和音频帧时非常有用，因为它可以有效地管理和转移帧数据，而无需进行数据的深度复制。
     */
    av_frame_move_ref(vp->frame,src_frame);//保持视频帧数据
    frame_queue_push(&scp->pictq);
    return 0;
}



//=========================== 解码视频保存到视频帧队列中 ===========================
void *video_decode_thread(void *arg){
    int ret = -1;
    
    double pts;
    double duration;
    
    SCPlayer *scp = (SCPlayer *)arg;
    AVFrame *video_frame = NULL;   /* 解码器输出（可能是 VT 硬解帧） */
    AVFrame *sw_frame = NULL;      /* 硬解 transfer / 软帧暂存 */
    AVFrame *yuv_frame = NULL;     /* 统一成 YUV420P 给现有 OpenGL 渲染 */
    AVFrame *frame_for_queue = NULL;
    
    AVRational tb = scp->video_st->time_base;
    AVRational frame_rate = av_guess_frame_rate(scp->ic,scp->video_st,NULL);//猜测视频流或帧的帧率（Frame Rate）
    
    video_frame = av_frame_alloc();
    sw_frame = av_frame_alloc();
    yuv_frame = av_frame_alloc();
    if (!video_frame || !sw_frame || !yuv_frame) {
        ret = AVERROR(ENOMEM);
        goto __ERROR;
    }
    
    for(;;){
        if(scp->quit){
            break;
        }
        //以非阻塞的方式从队列中获取包，如果没有就直接返回，并且等待10ms在取,直到读取取
        ret = packet_queue_get(&scp->videoq,&scp->video_pkt,0);
        if(ret <= 0){
            av_log(scp->video_ctx,AV_LOG_DEBUG,"video delay 10 ms\n");
            sc_delay_ms(10);
            continue;
        }
        //发送视频包给解码器
        ret = avcodec_send_packet(scp->video_ctx,&scp->video_pkt);
        if(ret < 0){
            av_log(scp->video_ctx,AV_LOG_DEBUG,"发送视频包给解码器失败！\n");
            goto __ERROR;
        }
        
        //轮询解码结果
        while(ret >= 0){
            ret = avcodec_receive_frame(scp->video_ctx,video_frame);
            if(ret == AVERROR(EAGAIN) || ret == AVERROR_EOF){
                break;//已经度到文件尾
            } else if ( ret < 0){
                /* 模拟器上 VT 常在首帧 setup 失败，解码器状态已坏，整路重开软解并重送当前包 */
                if (scp->hw_fallback || scp->video_ctx->hw_device_ctx) {
                    av_log(scp->video_ctx, AV_LOG_WARNING,
                           "VT decode failed (%s), reopen software and retry packet\n",
                           av_err2str(ret));
                    if (sc_video_reopen_software(scp) == 0) {
                        ret = avcodec_send_packet(scp->video_ctx, &scp->video_pkt);
                        if (ret >= 0) {
                            continue;
                        }
                    }
                }
                av_log(scp->video_ctx,AV_LOG_ERROR,"从解码器接受视频帧失败！\n");
                ret = -1;
                goto __ERROR;
            }

            if (scp->hw_fallback) {
                /* get_format 已回退但当前包可能已损坏，重开后再解 */
                av_frame_unref(video_frame);
                if (sc_video_reopen_software(scp) == 0) {
                    ret = avcodec_send_packet(scp->video_ctx, &scp->video_pkt);
                    if (ret >= 0) {
                        continue;
                    }
                }
                scp->hw_fallback = 0;
            }

            if (video_frame->format == AV_PIX_FMT_VIDEOTOOLBOX) {
                scp->hw_video = 1;
            }

            frame_for_queue = video_frame;
            /* 硬解输出为 AV_PIX_FMT_VIDEOTOOLBOX，先转到系统内存（多为 NV12） */
            if (video_frame->format == AV_PIX_FMT_VIDEOTOOLBOX) {
                av_frame_unref(sw_frame);
                ret = av_hwframe_transfer_data(sw_frame, video_frame, 0);
                if (ret < 0) {
                    av_log(scp->video_ctx, AV_LOG_ERROR,
                           "av_hwframe_transfer_data failed: %s\n", av_err2str(ret));
                    av_frame_unref(video_frame);
                    continue;
                }
                av_frame_copy_props(sw_frame, video_frame);
                frame_for_queue = sw_frame;
                av_frame_unref(video_frame);
            }

            /* OpenGL 路径按 Y/U/V 平面上传，统一转成 YUV420P */
            if (frame_for_queue->format != AV_PIX_FMT_YUV420P &&
                frame_for_queue->format != AV_PIX_FMT_YUVJ420P) {
                if (!scp->sws_ctx) {
                    scp->sws_ctx = sws_getContext(
                        frame_for_queue->width, frame_for_queue->height, frame_for_queue->format,
                        frame_for_queue->width, frame_for_queue->height, AV_PIX_FMT_YUV420P,
                        SWS_BILINEAR, NULL, NULL, NULL);
                    if (!scp->sws_ctx) {
                        av_log(scp->video_ctx, AV_LOG_ERROR, "sws_getContext failed\n");
                        av_frame_unref(frame_for_queue);
                        continue;
                    }
                }
                av_frame_unref(yuv_frame);
                yuv_frame->format = AV_PIX_FMT_YUV420P;
                yuv_frame->width = frame_for_queue->width;
                yuv_frame->height = frame_for_queue->height;
                ret = av_frame_get_buffer(yuv_frame, 32);
                if (ret < 0) {
                    av_frame_unref(frame_for_queue);
                    continue;
                }
                av_frame_copy_props(yuv_frame, frame_for_queue);
                sws_scale(scp->sws_ctx,
                          (const uint8_t * const *)frame_for_queue->data,
                          frame_for_queue->linesize, 0, frame_for_queue->height,
                          yuv_frame->data, yuv_frame->linesize);
                av_frame_unref(frame_for_queue);
                frame_for_queue = yuv_frame;
            }
            
            /*
             音视频同步相关
             */
            //视频帧持续的时间（ms）
            duration = (frame_rate.num && frame_rate.den ?
                        sc_sec_to_ms(av_q2d((AVRational){frame_rate.den,frame_rate.num})) : 0);
            scp->frame_duration = duration;
            //视频帧呈现时间（ms）
            pts = (frame_for_queue->pts == AV_NOPTS_VALUE) ? NAN : sc_ts_to_ms(frame_for_queue->pts, tb);
            pts = synchronize_video(scp,frame_for_queue,pts);//计算video clock 视频的播放时长，当前的video_clock + 1/tbr(帧率)
            
            /*
             insert FrameQueue kt_pos: 这是一个 64 位的整数，
             用于表示帧在原始输入文件中的字节偏移量，
             它通常对应于该帧所在的压缩数据包（AVPacket）的文件位置
             */
            //保存视频帧到queue中
            
            queue_pitcure(scp,frame_for_queue,pts,duration,frame_for_queue->pkt_pos);
            av_frame_unref(frame_for_queue);
        }
    }
    
    ret = 0;
__ERROR:
    av_frame_free(&video_frame);
    av_frame_free(&sw_frame);
    av_frame_free(&yuv_frame);
    return (void *)(intptr_t)ret;
}


//视频的clock（ms）
double get_video_clock(SCPlayer *scp){
    double delta = av_gettime_ms() - scp->video_current_pts_time;
    return scp->video_current_pts + delta;
}



/* 克隆后立即出队，与同步无关；busy 时丢弃本帧拷贝，不卡住刷新 */
static void video_dscpplay(SCPlayer *scp){
    Frame *vp;
    AVFrame *owned;

    if (scp->pictq.size == 0 || !scp->player_call_other) {
        return;
    }
    /* GL 忙：照样出队，不送显，避免拖慢同步/解码 */
    if (scp->display_busy) {
        fream_queue_pop(&scp->pictq);
        return;
    }
    if (scp->quit) {
        return;
    }
    vp = frame_queue_peek(&scp->pictq);
    if (!vp || !vp->frame) {
        return;
    }
    owned = av_frame_clone(vp->frame);
    fream_queue_pop(&scp->pictq);
    if (!owned) {
        return;
    }
    scp->display_busy = 1;
    scp->player_call_other(owned, 1, scp, scp->userData);//呼叫渲染模块
}

/* 视频落后超过约 1s 开始狂追 */
static void video_drop_late_frames(SCPlayer *scp, double frame_ms)
{
    const double nosync_threshold = 10000.0;
    Frame *vp;
    double diff;
    
    if (scp->av_sync_type != AV_SYNC_AUDIO_MASTER) {
        return;
    }
    if (isnan(scp->audio_clock)) {
        return;
    }
    
    while (scp->pictq.size > 1) {
        vp = frame_queue_peek(&scp->pictq);
        diff = vp->pts - get_maste_clock(scp);
        if (isnan(diff) || fabs(diff) <= nosync_threshold) {
            break;
        }
        fream_queue_pop(&scp->pictq);
        scp->frame_last_pts = vp->pts;
    }
}

void video_refresh_timer_tmp(void *userdata){
    
    SCPlayer *scp = (SCPlayer*)userdata;
    Frame *vp = NULL;
    double diff,ref_clock = 0;
    
    if(scp->video_st){
        
        if(scp->pictq.size == 0){//如果视频queue是空的，延时1毫秒 快速的检测
            scp->delay_video_time = 1;// 1ms 调用
        } else if (scp->av_sync_type == AV_SYNC_AUDIO_MASTER && isnan(scp->audio_clock)) {
            scp->delay_video_time = 5;//音频时钟尚未建立：先别开跑，避免启动阶段乱追
        } else {
            //下一帧视频播放时间
            vp = frame_queue_peek(&scp->pictq);//读起视频帧
            ref_clock = get_maste_clock(scp);//播放到的音频时刻（ms）
            diff = vp->pts - ref_clock;
            scp->delay_video_time = diff;
            if(diff <= 0){
                diff = 0;
            }
            video_dscpplay(scp);
            printf("diff = %.2f ms \n",diff);
        }
    } else {
        scp->delay_video_time = 100;//等待打开视频流
    }
    
}

//刷新视频帧
void video_refresh_timer(void *userdata){
    return video_refresh_timer_tmp(userdata);
    
    SCPlayer *scp = (SCPlayer*)userdata;
    Frame *vp = NULL;
    double actual_delay,delay,sync_threshold,ref_clock,diff = 0;
    double frame_ms = sc_video_frame_ms(scp); //视频帧默认时间
    
    if(scp->video_st){
        
        if(scp->pictq.size == 0){//如果视频queue是空的，延时1毫秒 快速的检测
            scp->delay_video_time = 1;// 1ms 调用
        } else if (scp->av_sync_type == AV_SYNC_AUDIO_MASTER && isnan(scp->audio_clock)) {//音频时钟尚未建立：先别开跑，避免启动阶段乱追
            scp->delay_video_time = 5;
        } else {
            /* 若已明显落后，先丢到接近音频再算 delay */
            video_drop_late_frames(scp, frame_ms);
           
            
            //计算下一帧的显示时间
            vp = frame_queue_peek(&scp->pictq);//读起视频帧
            // printf("PTS = %f ms\n",vp->pts);
            scp->video_current_pts = vp->pts;//对scp的赋值，将要播放的视频帧的pts
            scp->video_current_pts_time = av_gettime_ms();//但前时钟（ms）
            
            if(scp->frame_last_pts == 0){//一开始时 frame_last_pts 是为 0
                delay = frame_ms;
            } else {
                delay = vp->pts - scp->frame_last_pts;//将要播放的一帧和上一帧的时间间隔（ms）
            }
            
            /* 非法间隔：<=0 或 >=1s(1000ms) 时回退到上一帧 delay，再不行用 fps 帧间隔 */
            if(delay <= 0 || delay >= 1000.0){
                delay = (scp->frame_last_delay > 0) ? scp->frame_last_delay : frame_ms;
            }
            //跟新frame_last_delay，frame_last_pts
            scp->frame_last_delay = delay;
            scp->frame_last_pts = vp->pts;
            
            //推算下一帧视频时间和音频同步，计算delay来同步
            if(scp->av_sync_type == AV_SYNC_AUDIO_MASTER){
                ref_clock = get_maste_clock(scp);//播放到的音频时刻（ms）
                diff = vp->pts - ref_clock;//将要播放的视频帧 - 播放到的音频时刻 (正常情况下将要播放的视频帧要等diff才可以播放)
            }
            
            /* Skip or repeat the frame. Take delay into account
             FFPlay still doesn't "know if thscp scp the best guess."
             对齐 ffplay compute_target_delay（本工程时间单位为 ms）
             
             sync_threshold = clamp(delay, 40ms, 100ms)
             落后：delay = max(0, delay + diff)
             超前且 delay > 100ms：delay += diff（长帧直接补差，不翻倍）
             超前且 delay <= 100ms：delay *= 2（短帧用翻倍拉长显示）
             */
            const double sync_threshold_min = 40.0;   /* AV_SYNC_THRESHOLD_MIN  0.04s */
            const double sync_threshold_max = 100.0;  /* AV_SYNC_THRESHOLD_MAX  0.1s  */
            const double framedup_threshold = 100.0;  /* AV_SYNC_FRAMEDUP_THRESHOLD 0.1s */
            const double nosync_threshold = 10000.0;  /* AV_NOSYNC_THRESHOLD 10s */
            
            sync_threshold = FFMAX(sync_threshold_min, FFMIN(sync_threshold_max, delay));
            
            if (!isnan(diff) && fabs(diff) < nosync_threshold) {
                if (diff <= -sync_threshold) {
                    /* 视频落后：缩短等待 */
                    delay = FFMAX(0.0, delay + diff);//慢一点追平滑,不要一下跳过去
                } else if (diff >= sync_threshold && delay > framedup_threshold) {
                    /* 长帧超前：直接加上 diff */
                    delay = delay + diff;
                } else if (diff >= sync_threshold) {
                    /* 短帧超前：翻倍等待（重复显示） */
                    delay = 2.0 * delay;
                }
            }
            
            
            printf("DIFF = %f ms  delay = %f ms \n", diff, delay);
            
            scp->frame_timer += delay;//推算出下一帧的显示时间点（ms）
            /* computer the REAL delay
             一帧确定好系统时间后，后面就将要播放的帧的时间换算成系统时间，
             如果发现要播放的帧的时间落后于系统时间就将其播放出来。
             */
            actual_delay = scp->frame_timer - av_gettime_ms();
            if (actual_delay < 0) {
                actual_delay = 0;
            }
            scp->delay_video_time = actual_delay;
            video_dscpplay(scp);
        }
        
    } else {
        scp->delay_video_time = 100;//等待打开视频流
    }
    
}
