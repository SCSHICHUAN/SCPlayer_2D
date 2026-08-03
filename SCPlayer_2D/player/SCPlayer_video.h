//
//  SCPlayer_video.h
//  SCPlayer_2D
//
//  Created by Stan on 2026/8/3.
//  Copyright © 2026 石川. All rights reserved.
//

#ifndef SCPlayer_video_h
#define SCPlayer_video_h

#include "SCPlayer.h"

int video_decode_thread(void *arg);
void video_refresh_timer(void *userdata);
double get_video_clock(VideoState *is);

#endif /* SCPlayer_video_h */
