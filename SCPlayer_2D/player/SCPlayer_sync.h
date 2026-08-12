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

void video_refresh_timer_audio_clock(void *userdata);
void video_refresh_timer_external_clock(void *userdata);



#endif /* SCPlayer_sync_h */


