//
//  SCPlayer_audio.h
//  SCPlayer_2D
//
//  Created by Stan on 2026/8/3.
//  Copyright © 2026 石川. All rights reserved.
//

#ifndef SCPlayer_audio_h
#define SCPlayer_audio_h

#include "SCPlayer.h"

int audio_open(void *opaque,
               AVChannelLayout *wanted_channel_layout,
               int wented_salple_rate);
void sdl_audio_callback_1(void *userdata, uint8_t *stream, int len);
double get_audio_clock(VideoState *is);

#endif /* SCPlayer_audio_h */
