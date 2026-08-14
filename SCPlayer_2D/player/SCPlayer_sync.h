//
//  SCPlayer_sync.h
//  SCPlayer_2D
//
//  Created by Stan on 2026/8/12.
//  Copyright © 2026 石川. All rights reserved.
//

#ifndef SCPlayer_sync_h
#define SCPlayer_sync_h

#include <stdio.h>
#include "SCPlayer.h"
#include "SCPlayer_audio.h"
#include "SCPlayer_video.h"

#define SC_NOSYNC_THRESHOLD_MS    10000.0

void external_clock_init(SCPlayer *scp);
void external_clock_set(SCPlayer *scp, double pts_ms);
double get_external_clock(SCPlayer *scp);
void external_clock_sync_to_slave(SCPlayer *scp, double slave_pts_ms);

void video_refresh_timer_audio_clock(void *userdata);
void video_refresh_timer_external_clock(void *userdata);

#endif /* SCPlayer_sync_h */
