//
//  SCPlayer_sync.c
//  SCPlayer_2D
//
//  Created by Stan on 2026/8/12.
//  Copyright © 2026 石川. All rights reserved.
//

#include "SCPlayer_sync.h"
#include <math.h>

void external_clock_init(SCPlayer *scp)
{
    if (!scp) {
        return;
    }
    scp->external_pts = NAN; /* 未铆 */
}

void external_clock_set(SCPlayer *scp, double pts_ms)
{
    (void)pts_ms;
    if (!scp) {
        return;
    }
    scp->external_pts = av_gettime_ms(); /* 铆点 */
}

/* external_pts(钟) = now - 铆点 */
double get_external_clock(SCPlayer *scp)
{
    if (!scp || isnan(scp->external_pts)) {
        return NAN;
    }
    return av_gettime_ms() - scp->external_pts;
}

/* 只在尚未铆住时钉一次 */
void external_clock_sync_to_slave(SCPlayer *scp, double slave_pts_ms)
{
    if (!scp || isnan(slave_pts_ms)) {
        return;
    }
    if (isnan(scp->external_pts)) {
        external_clock_set(scp, slave_pts_ms);
    }
}

//音频主时钟同步
void video_refresh_timer_audio_clock(void *userdata){

    SCPlayer *scp = (SCPlayer*)userdata;
    Frame *vp = NULL;
    double diff,ref_clock = 0;

    if(scp->video_st){

        if(scp->pictq.size == 0){//如果视频queue是空的，延时1毫秒 快速的检测
            scp->delay_video_time = 1;// 1ms 调用
        } else if (isnan(scp->audio_clock)) {
            scp->delay_video_time = 5;//音频时钟尚未建立：先别开跑，避免启动阶段乱追
        } else {
            //下一帧视频播放时间
            vp = frame_queue_peek(&scp->pictq);//读起视频帧
            ref_clock = get_audio_clock(scp);//播放到的音频时刻（ms）
            diff = vp->pts - ref_clock;
            if(diff <= 0){
                diff = 0;
            }
            scp->delay_video_time = diff;
            video_dscpplay(scp);
//            printf("diff = %.2f ms \n",diff);
        }
        
    } else {
        scp->delay_video_time = 100;//等待打开视频流
    }

}

/* EXTERNAL：视频仍跟音频钟 */
void video_refresh_timer_external_clock(void *userdata){
     video_refresh_timer_audio_clock(userdata);
}

