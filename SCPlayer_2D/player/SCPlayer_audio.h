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

int audio_wanted_spec(void *opaque,
               AVChannelLayout *wanted_channel_layout,
               int wented_salple_rate);
void audio_decode_callback(void *userdata, uint8_t *stream, int len);
void audio_queue_wrote(SCPlayer *scp, int bytes);
void audio_queue_consumed(SCPlayer *scp, int bytes);
void audio_aq_interp_start(SCPlayer *scp);
void audio_aq_interp_stop(SCPlayer *scp);
void audio_aq_set_paused(SCPlayer *scp, int paused);
double get_audio_clock(SCPlayer *scp);

#endif /* SCPlayer_audio_h */
