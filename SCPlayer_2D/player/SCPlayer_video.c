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
    frame_delay = av_q2d(is->video_ctx->time_base);
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
int video_decode_thread(void *arg){
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
            SDL_Delay(10);
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
             //视频帧持续的时间
             duration = (frame_rate.num && frame_rate.den ?
                         av_q2d((AVRational){frame_rate.den,frame_rate.num}) : 0);
             is->frame_duration = duration;
             //视频帧的呈现的时间，这里是以秒为单位的时间
             pts = (video_frame->pts == AV_NOPTS_VALUE) ? NAN : video_frame->pts * av_q2d(tb);
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
  return ret;
}

/*
DAR（Display Aspect Ratio，显示长宽比）：视频显示的宽高比。
SAR（Sample Aspect Ratio，样本长宽比）：视频帧中每个像素的宽高比。
PAR（Pixel Aspect Ratio，像素长宽比）：与 SAR 等同，用于描述每个像素的宽高比。

在这种情况下，视频文件在显示设备上播放时，需要进行调整以匹配设备的像素特性，这可能会导致 SAR 和 PAR 的不一致。
假设有一个视频文件，其编码信息指示 SAR 为 1:1（即像素为正方形），
但该视频是为了在具有非正方形像素的显示设备上播放。在这种情况下，视频文件的 SAR 和实际显示设备的 PAR 可能会不一致。具体地：

视频文件的 SAR 为 1:1（正方形像素）
显示设备的 PAR 为 4:3（长方形像素）
在这种情况下，视频文件在显示设备上播放时，需要进行调整以匹配设备的像素特性，
这可能会导致 SAR 和 PAR 的不一致。

int scr_xleft：屏幕左上角的 x 坐标。
int scr_ytop：屏幕左上角的 y 坐标。
int scr_width：设置宽度。
int scr_height：设置的高度。
int pic_width：视频帧的宽度。
int pic_height：视频帧的高度。
AVRational pic_sar：视频帧的样本长宽比（SAR）。
 */

//static void calculate_display_rect(SDL_Rect *rect,
//                                    int scr_xleft,int scr_ytop,
//                                    int scr_width,int scr_height,
//                                    int pic_width,int pic_height,
//                                    AVRational pic_sar){
//    //  pic_sar.num = 4,pic_sar.den = 3;
//    AVRational aspect_ratio = pic_sar;
//    int64_t width, height, x, y;
//   // aspect_ratio 无效（小于等于 0:1），则将其设置为 1:1（即像素为正方形）。
//    if (av_cmp_q(aspect_ratio, av_make_q(0, 1)) <= 0)
//        aspect_ratio = av_make_q(1, 1);
//
//    /*
//      假设一个视频帧的宽度为 720 像素，高度为 480 像素，SAR（或 PAR）为 4:3。
//
//                                    宽度            720      4
//       视频帧的宽高比（DAR）为：DAR = ------- × SAR = ------ x --- = 2
//                                    高度            480      3
//     */
//    aspect_ratio = av_mul_q(aspect_ratio, av_make_q(pic_width, pic_height));
//
//    /* XXX: we suppose the screen has a 1.0 pixel ratio */
//    /*
//    高度添满
//    查看宽度释放适合
//    */
//    height = scr_height;
//    width = av_rescale(height, aspect_ratio.num, aspect_ratio.den) & ~1;//av_rescale(int64_t a, int64_t b, int64_t c) a * b / c
//    if (width > scr_width) {
//        width = scr_width;
//        /*
//        1.“~1” 是对整数 1 进行按位取反操作。在 32 位整数表示中，
//        2.“1” 表示为 00000000 00000000 00000000 00000001，
//        按位取反后的结果是 11111111 11111111 11111111 11111110，即 0xFFFFFFFE。
//
//        3.按位与操作符 & 会对两个数的每一位执行与操作，即只有当两个数的对应位都是 1 时，结果才是 1，否则为 0。
//
//        4.将 1923 与 0xFFFFFFFE 进行按位与操作：
//
//                                  11110000011
//           & 11111111111111111111111111111110
//          --------------------------------------
//                                  11110000010
//           11110000010 = 1922
//         5.原数的最低位变成了 0，即 1923 被转换为 1922，确保了结果是偶数,设备显示需要
//        */
//        height = av_rescale(width, aspect_ratio.den, aspect_ratio.num) & ~1;
//    }
//
//    printf("sar = %d/%d,aspect_ratio = %d/%d,d_width/d_height = %d/%d pic_w/pic_h = %d/%d \n",
//    pic_sar.num,pic_sar.den,aspect_ratio.num,aspect_ratio.den,(int)width,(int)height,pic_width,pic_height);
//
//   //计算frame x y
//   x = (scr_width - width) / 2;
//   y = (scr_height - height) /2;
//   rect->x = scr_xleft + x;
//   rect->y = scr_ytop + y;
//   //计算frame的 宽高
//   rect->w = FFMAX((int)width,1);
//   rect->h = FFMAX((int)height,1);
//
//}
//
//static void set_default_window_size(int width,int height,AVRational sar){
//    SDL_Rect rect;
//    int max_width = screen_width ? screen_width : INT_MAX;
//    int max_height = screen_height ? screen_height : INT_MAX;
//    if(max_width == INT_MAX && max_height == INT_MAX)
//       max_height = height;
//    calculate_display_rect(&rect,0,0,max_width,max_height,width,height,sar);
//    default_width = rect.w;
//    default_height = rect.h;
//}

//视频的clock
double get_video_clock(VideoState *is){
    double delta;

    delta = av_gettime() - is->video_current_pts_time;
    delta = delta/ch_µs_to_s;
    return is->video_current_pts_time + delta;
}

//    int w,h;

//    w = screen_width ? screen_width : default_width;
//    h = screen_height ? screen_height : default_height;
    
//    if(!window_title)
//        window_title = input_filename;
//    SDL_SetWindowTitle(win,window_title);
//
//    SDL_SetWindowSize(win,w,h);
//    SDL_SetWindowPosition(win,screen_left,screen_top);
//    if(is_full_screen)
//        SDL_SetWindowFullscreen(win,SDL_WINDOW_FULLSCREEN_DESKTOP);
//    SDL_ShowWindow(win);

//    is->width = w;
//    is->height = h;

static void video_display(VideoState *is){
      Frame *vp = NULL;
      AVFrame *frame = NULL;
      vp = frame_queue_peek(&is->pictq);
      frame = vp->frame;
      is->fn_call(frame,1,is);
}

//刷新视频帧
void video_refresh_timer(void *userdata){

    VideoState *is = (VideoState*)userdata;
    Frame *vp = NULL;
    
   

    double actual_delay,delay,sync_threshold,ref_clock,diff = 0;

    if(is->video_st){
       
        if(is->pictq.size == 0){
            /*
            if the queue is empty, so we shoud be as fast as checking queue of picture
            如果视频queue是空的，延时1毫秒 快速的检测
            */
            schedule_refresh(is,1);
        } else {
            /*
             计算下一帧的显示时间
            */

            vp = frame_queue_peek(&is->pictq);//队列中取到要渲染的frame 生产 > 消费，一般都可以读取到帧
            
            printf("PTS = %f \n",vp->pts);
            
            is->video_current_pts = vp->pts;//对is的域赋值，当前video的pts
            is->video_current_pts_time = av_gettime();//当前视频帧显示时间

            if(is->frame_last_pts == 0){//一开始时 frame_last_pts 是为 0
                delay = 0;
            } else {
                //视频帧的时间间隔和下一帧。当前的pts - 上一帧的pts
                delay = vp->pts - is->frame_last_pts;
            }

            if(delay <= 0 || delay >= 1.0){
                delay = is->frame_last_delay;
            }

            //计算完成，跟新frame_last_delay，frame_last_pts
            is->frame_last_delay = delay;
            is->frame_last_pts = vp->pts;

            //推算下一帧视频时间和音频同步，计算delay来同步
            if(is->av_sync_type == AV_SYNC_AUDIO_MASTER){
               ref_clock = get_maste_clock(is);
               diff = vp->pts - ref_clock;//视频时间下一帧的 - 音频时间
            }

          /* Skip or repeat the frame. Take delay into account
          FFPlay still doesn't "know if this is the best guess."

                sync_threshold   视频当前帧和下一帧的时间比较  视频自己的delay
                          diff   是视频时间和音频时间比较     视频和音频

           视频时间 - 音频audio_clock的时间     当前的pts - 上一帧的pts，
           vp->pts - ref_clock               vp->pts - is->frame_last_pts
                  |                                  |
                diff                               delay

            ---------------- 0 ---------------> x
              -3      -1           1       3
             diff   delay        delay   diff

             以diff为准来修正delay
           */
            sync_threshold = (delay > 0.01) ? delay : 0.01;
            if(fabs(diff) < 10.0){//在 10 ms内认为同步
                if(diff <= - sync_threshold){
                    delay = 0;
                }else if(diff >= sync_threshold){//视频播放到前面要等待
                    delay = 2*delay;
                }
            }


            is->frame_timer += delay;//推算出下一帧的显示时间点,这个是和系统时间维护的一个时间
            /* computer the REAL delay
             一帧确定好系统时间后，后面就将要播放的帧的时间换算成系统时间，
              如果发现要播放的帧的时间落后于系统时间就将其播放出来。
            */
            //查看这一帧是不是要显示，对比推算的时间和当前时间，如果推算的时间等于当前时间，立刻马上显示
            actual_delay = is->frame_timer - (av_gettime()/ch_µs_to_s);
            if(actual_delay < 0.010){
                
//                if(current_PTS == front_PTS){//pts 是累加的不回相等，相等出错了
//                    actual_delay = is->frame_duration;//视频帧持续的时间
//                }else{
                    actual_delay = 0.010;
//                }
            }

            
            
            //到下个时间点刷新
            schedule_refresh(is,(int)(actual_delay * 1000 + 0.5));
            printf("schedule_refresh_time= %dms\n",(int)(actual_delay * 1000 + 0.5));

           /* show the picture!
           下一帧的时间间隔计算结束
           展示当前帧
           */
//           schedule_refresh(is,40);
            is->delay_video_time = (int)(actual_delay * 1000 + 0.5);
            printf("actual_delay:%f\n",actual_delay);
           
            if(actual_delay == 0.010){
                fream_queue_pop(&is->pictq);//时间太短不渲染
            }else{
                video_display(is);
            }
        }
    } else {
        schedule_refresh(is,100);//等待打开视频流
    }

}
