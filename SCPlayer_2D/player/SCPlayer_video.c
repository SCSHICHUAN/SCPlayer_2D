//
//  SCPlayer_video.c
//  SCPlayer_2D
//
//  Created by Stan on 2026/8/3.
//  Copyright © 2026 石川. All rights reserved.
//

#include "SCPlayer_video.h"
#include <math.h>

//推算pts 因为有时会没有pts
static double synchronize_video(VideoState *is,AVFrame *sec_frame,double pts){
    double frame_delay;
    
    if(pts != 0){
        is->video_clock = pts;
    } else {
        //如果这一帧没有pts，说明这一帧播放的时间不确定，把之前保存的video_clock给pts
        pts = is->video_clock;
    }
    /* time_base 单位为秒，产出转为 ms */
    frame_delay = sc_sec_to_ms(av_q2d(is->video_ctx->time_base));
    /* if we are repeating a frame, adjust clock accordingly
     重复帧用于平滑视频播放，并调整帧率
     重复帧（repeat_pict）是一个表示当前帧需要重复显示的次数的值，一般为0不重复
     帧率平滑：通过使用半帧时间的增加量，视频解码器可以更准确地分配时间戳，
     以平滑帧率转换。例如，从 24 FPS 转换到 30 FPS，或其他帧率转换。
     */
    frame_delay += sec_frame->repeat_pict * (frame_delay * 0.5);
    
    is->video_clock += frame_delay;
    return pts;
}
//保存到帧队列中
static int queue_pitcure(VideoState *is,AVFrame *src_frame,
                         double pts,double duration,int64_t pos){
    Frame *vp;
    if(!(vp = frame_queue_peek_writable(&is->pictq)))
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
    frame_queue_push(&is->pictq);
    return 0;
}



//=========================== 解码视频保存到视频帧队列中 ===========================
void *video_decode_thread(void *arg){
    int ret = -1;
    
    double pts;
    double duration;
    
    VideoState *is = (VideoState *)arg;
    AVFrame *video_frame = NULL;
    //    Frame *vp = NULL;
    
    AVRational tb = is->video_st->time_base;
    AVRational frame_rate = av_guess_frame_rate(is->ic,is->video_st,NULL);//猜测视频流或帧的帧率（Frame Rate）
    
    video_frame = av_frame_alloc();
    
    for(;;){
        if(is->quit){
            break;
        }
        //以非阻塞的方式从队列中获取包，如果没有就直接返回，并且等待10ms在取,直到读取取
        ret = packet_queue_get(&is->videoq,&is->video_pkt,0);
        if(ret <= 0){
            av_log(is->video_ctx,AV_LOG_DEBUG,"video delay 10 ms\n");
            sc_delay_ms(10);
            continue;
        }
        //发送视频包给解码器
        ret = avcodec_send_packet(is->video_ctx,&is->video_pkt);
        if(ret < 0){
            av_log(is->video_ctx,AV_LOG_DEBUG,"发送视频包给解码器失败！\n");
            goto __ERROR;
        }
        
        //轮询解码结果
        while(ret >= 0){
            ret = avcodec_receive_frame(is->video_ctx,video_frame);
            if(ret == AVERROR(EAGAIN) || ret == AVERROR_EOF){
                break;//已经度到文件尾
            } else if ( ret < 0){
                av_log(is->video_ctx,AV_LOG_ERROR,"从解码器接受视频帧失败！\n");
                ret = -1;
                goto __ERROR;
            }
            
            //            is->fn(video_frame,0);
            
            /*
             音视频同步相关
             */
            //视频帧持续的时间（ms）
            duration = (frame_rate.num && frame_rate.den ?
                        sc_sec_to_ms(av_q2d((AVRational){frame_rate.den,frame_rate.num})) : 0);
            is->frame_duration = duration;
            //视频帧呈现时间（ms）
            pts = (video_frame->pts == AV_NOPTS_VALUE) ? NAN : sc_ts_to_ms(video_frame->pts, tb);
            pts = synchronize_video(is,video_frame,pts);//计算video clock 视频的播放时长，当前的video_clock + 1/tbr(帧率)
            
            /*
             insert FrameQueue kt_pos: 这是一个 64 位的整数，
             用于表示帧在原始输入文件中的字节偏移量，
             它通常对应于该帧所在的压缩数据包（AVPacket）的文件位置
             */
            //保存视频帧到queue中
            
            queue_pitcure(is,video_frame,pts,duration,video_frame->pkt_pos);
            av_frame_unref(video_frame);
        }
    }
    
    ret = 0;
__ERROR:
    av_frame_free(&video_frame);
    return (void *)(intptr_t)ret;
}


//视频的clock（ms）
double get_video_clock(VideoState *is){
    double delta = sc_gettime_ms() - is->video_current_pts_time;
    return is->video_current_pts + delta;
}



static void video_display(VideoState *is){
    Frame *vp = NULL;
    AVFrame *frame = NULL;
    vp = frame_queue_peek(&is->pictq);
    frame = vp->frame;
    is->fn_call(frame,1,is,is->userData);
}

/* 视频落后超过约 1 帧时，一次丢到对齐音频，避免 1ms 连丢把 pts 冲到音频前面 */
static void video_drop_late_frames(VideoState *is, double frame_ms)
{
    const double nosync_threshold = 10000.0;
    Frame *vp;
    double diff;
    
    if (is->av_sync_type != AV_SYNC_AUDIO_MASTER) {
        return;
    }
    if (isnan(is->audio_clock)) {
        return;
    }
    
    while (is->pictq.size > 1) {
        vp = frame_queue_peek(&is->pictq);
        diff = vp->pts - get_maste_clock(is);
        if (isnan(diff) || fabs(diff) >= nosync_threshold) {
            break;
        }
        /* 落后不到一帧：留给后面的 delay 微调 */
        if (diff >= -frame_ms) {
            break;
        }
        fream_queue_pop(&is->pictq);
        is->frame_last_pts = vp->pts;
    }
}

//刷新视频帧
void video_refresh_timer(void *userdata){
    
    VideoState *is = (VideoState*)userdata;
    Frame *vp = NULL;
    double actual_delay,delay,sync_threshold,ref_clock,diff = 0;
    double frame_ms = sc_video_frame_ms(is);
    
    if(is->video_st){
        
        if(is->pictq.size == 0){
            //如果视频queue是空的，延时1毫秒 快速的检测
            is->delay_video_time = 1;// 1ms 调用
            is->frame_display_pending = 0;
        } else if (is->frame_display_pending) {
            /* 队头已送显：不再重复累加 delay，只按 frame_timer 等到点 / 等 GL 出队 */
            actual_delay = is->frame_timer - sc_gettime_ms();
            if (actual_delay < 1.0) {
                actual_delay = 1.0;
            }
            is->delay_video_time = (uint32_t)(actual_delay + 0.5);
        } else if (is->av_sync_type == AV_SYNC_AUDIO_MASTER && isnan(is->audio_clock)) {
            /* 音频时钟尚未建立：先别开跑，避免启动阶段乱追 */
            is->delay_video_time = 5;
        } else {
            /* 若已明显落后，先丢到接近音频再算 delay */
            video_drop_late_frames(is, frame_ms);
            if (is->pictq.size == 0) {
                is->delay_video_time = 1;
                return;
            }
            
            //计算下一帧的显示时间
            
            vp = frame_queue_peek(&is->pictq);//读起视频帧
            
            //            printf("PTS = %f ms\n",vp->pts);
            
            is->video_current_pts = vp->pts;//对is的域赋值，将要播放的视频帧的pts
            is->video_current_pts_time = sc_gettime_ms();//当前墙钟（ms）
            
            if(is->frame_last_pts == 0){//一开始时 frame_last_pts 是为 0
                delay = frame_ms;
            } else {
                //将要播放的一帧和上一帧的时间间隔（ms）
                delay = vp->pts - is->frame_last_pts;
            }
            
            /* 非法间隔：<=0 或 >=1s(1000ms) 时回退到上一帧 delay，再不行用 fps 帧间隔 */
            if(delay <= 0 || delay >= 1000.0){
                delay = (is->frame_last_delay > 0) ? is->frame_last_delay : frame_ms;
            }
            
            //跟新frame_last_delay，frame_last_pts
            is->frame_last_delay = delay;
            is->frame_last_pts = vp->pts;
            
            //推算下一帧视频时间和音频同步，计算delay来同步
            if(is->av_sync_type == AV_SYNC_AUDIO_MASTER){
                ref_clock = get_maste_clock(is);//播放到的音频时刻（ms）
                diff = vp->pts - ref_clock;//将要播放的视频帧 - 播放到的音频时刻 (正常情况下将要播放的视频帧要等diff才可以播放)
            }
            
            /* Skip or repeat the frame. Take delay into account
             FFPlay still doesn't "know if this is the best guess."
             
             sync_threshold
             
             视频将要播放帧  -  音频时间        视频将要播放帧  -  上一帧
             vp->pts - ref_clock            vp->pts - is->frame_last_pts
             |                              |
             diff                            delay
             
             ---------------- 0 ---------------> x
             -3      -1           1       3
             diff   delay        delay   diff
             
             以diff为准来修正delay（单位均为 ms）
             */
            
            
            const double nosync_threshold = 10000.0; /* |diff| 过大视为不连续，放弃同步 */
            double max_delay = frame_ms * 2.0;       /* 超前最多多等约 2 帧，避免 delay 飙到 100ms+ */
            
            /* 同步阈值取半帧：30fps→≈16.7ms；拿不到 fps 时半默认帧→20ms */
            sync_threshold = frame_ms * 0.5;
            
            if (!isnan(diff) && fabs(diff) < nosync_threshold) {
                if (diff <= -sync_threshold) {
                    /* 视频落后：缩短等待（大落后已在 video_drop_late_frames 处理） */
                    delay = FFMAX(0.0, delay + diff);
                } else if (diff >= sync_threshold) {
                    /* 视频超前：多等 diff，对齐音频（替代原来的 2*delay） */
                    delay = delay + diff;
                    if (delay > max_delay) {
                        delay = max_delay;
                    }
                }
            }
            
            
            printf("DIFF = %f ms  delay = %f ms \n", diff, delay);
            
            is->frame_timer += delay;//推算出下一帧的显示时间点（ms）
            /* computer the REAL delay
             一帧确定好系统时间后，后面就将要播放的帧的时间换算成系统时间，
             如果发现要播放的帧的时间落后于系统时间就将其播放出来。
             */
            actual_delay = is->frame_timer - sc_gettime_ms();
            {
                /* 最短休眠：取帧间隔的 1/4，至少 1ms */
                double min_sleep = frame_ms * 0.25;
                int too_late;
                
                if (min_sleep < 1.0) {
                    min_sleep = 1.0;
                }
                too_late = (actual_delay < min_sleep);
                if (too_late) {
                    actual_delay = min_sleep;
                }
                
                is->delay_video_time = (uint32_t)(actual_delay + 0.5);
                
                if(too_late){
                    fream_queue_pop(&is->pictq);//时间太短不渲染
                    /* 丢掉 timer 欠债，避免连续 too_late 把后续帧全丢光再冲过音频 */
                    is->frame_timer = sc_gettime_ms();
                    is->frame_display_pending = 0;
                    is->delay_video_time = 1;
                }else{
                    video_display(is);
                    /* GL 异步上传纹理，出队在回调里；期间勿重复处理同一帧 */
                    is->frame_display_pending = 1;
                }
            }
        }
        
    } else {
        is->delay_video_time = 100;//等待打开视频流
    }
    
}
