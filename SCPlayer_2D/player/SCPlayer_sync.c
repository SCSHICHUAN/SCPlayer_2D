//
//  SCPlayer_sync.c
//  SCPlayer_2D
//
//  Created by Stan on 2026/8/12.
//  Copyright © 2026 石川. All rights reserved.
//

#include "SCPlayer_sync.h"
#include <math.h>

void sc_ext_clock_init(SCPlayer *scp)
{
    if (!scp) {
        return;
    }
    scp->ext_pts = NAN;
    scp->ext_pts_drift = NAN;
    scp->ext_last_updated = 0;
}

void sc_ext_clock_set(SCPlayer *scp, double pts_ms)
{
    double time;
    if (!scp || isnan(pts_ms)) {
        return;
    }
    time = av_gettime_ms();
    scp->ext_pts = pts_ms;
    scp->ext_last_updated = time;
    scp->ext_pts_drift = pts_ms - time;
}

/* 无 speed：钉住后只跟墙钟 1:1 推进 */
double sc_ext_clock_get(SCPlayer *scp)
{
    if (!scp || isnan(scp->ext_pts_drift)) {
        return NAN;
    }
    return scp->ext_pts_drift + av_gettime_ms();
}

/* 只在外部钟尚未钉住时用 slave 初始化；之后外部钟只跟墙钟，不被音频拉回 */
void sc_ext_clock_sync_to_slave(SCPlayer *scp, double slave_pts_ms)
{
    if (!scp || isnan(slave_pts_ms)) {
        return;
    }
    if (isnan(sc_ext_clock_get(scp))) {
        sc_ext_clock_set(scp, slave_pts_ms);
    }
}

//音频主时钟同步（视频逻辑不改）
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
            ref_clock = get_maste_clock(scp);//播放到的音频时刻（ms）
            scp->audio_ref_clock = ref_clock;
            diff = vp->pts - ref_clock;
            scp->delay_video_time = diff;
            if(diff <= 0){
                diff = 0;
            }
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
