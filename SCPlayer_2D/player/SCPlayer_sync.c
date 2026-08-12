//
//  SCPlayer_sync.c
//  SCPlayer_2D
//
//  Created by Stan on 2026/8/12.
//  Copyright © 2026 石川. All rights reserved.
//

#include "SCPlayer_sync.h"

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
//音视频同步到外部时钟
void video_refresh_timer_external_clock(void *userdata){
   
    SCPlayer *scp = (SCPlayer*)userdata;
    
}
