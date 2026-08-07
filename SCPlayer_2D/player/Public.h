//
//  Public.h
//  SCFFmpeg
//
//  Created by Stan on 2025/3/24.
//  Copyright © 2025 石川. All rights reserved.
//

#ifndef Public_h
#define Public_h

#include <stdio.h>

#define FF_REFRESH_EVENT (100)
#define FF_QUIT_EVENT (100 + 1)

#define MAX_QUEUE_SIZE (5 * 1024 * 1024)
#define AUDIO_BUFFER_SIZE 1024
/*
 工程内媒体时间、同步时钟统一为毫秒 (ms)
 1 s = 1000 ms = 1000000 µs
 */
#define SC_S_TO_MS     1000.0
#define ch_µs_to_ms    1000.0
/* 拿不到视频 fps 时的默认帧间隔（约 25fps） */
#define SC_DEFAULT_FRAME_DURATION_MS 40.0

#define VIDEO_PICTURE_QUEUE_SIZE 3
#define SAMPLE_QUEUE_SIZE 9
#define FRAME_QUEUE_SIZE FFMAX(SAMPLE_QUEUE_SIZE, VIDEO_PICTURE_QUEUE_SIZE)
/*让线程挂起休眠；入参单位：毫秒 (ms)
 1 秒 = 1000 毫秒 (ms) = 1000000 微秒 (μs)*/
static inline void sc_delay_ms(double ms) {
    if (ms > 0) {
        usleep((useconds_t)(ms * 1000.0));
    }
}

/* FFmpeg 时间基时间戳 → 毫秒 */
static inline double sc_ts_to_ms(int64_t ts, AVRational tb) {
    return ts * av_q2d(tb) * SC_S_TO_MS;
}

/* 秒 → 毫秒 */
static inline double sc_sec_to_ms(double sec) {
    return sec * SC_S_TO_MS;
}

/* 当前墙钟时间（毫秒） */
static inline double av_gettime_ms(void) {
    return av_gettime() / ch_µs_to_ms;
}

/* 由视频流 fps 计算帧间隔(ms)；获取失败则返回默认值 */
static inline double sc_frame_duration_from_stream(AVFormatContext *ic, AVStream *st) {
    AVRational fr;
    double fps = 0;

    if (!st) {
        return SC_DEFAULT_FRAME_DURATION_MS;
    }

    fr = av_guess_frame_rate(ic, st, NULL);
    if (fr.num > 0 && fr.den > 0) {
        fps = av_q2d(fr);
    } else if (st->avg_frame_rate.num > 0 && st->avg_frame_rate.den > 0) {
        fps = av_q2d(st->avg_frame_rate);
    } else if (st->r_frame_rate.num > 0 && st->r_frame_rate.den > 0) {
        fps = av_q2d(st->r_frame_rate);
    }

    if (fps > 1.0 && fps < 120.0) {
        return SC_S_TO_MS / fps;
    }
    return SC_DEFAULT_FRAME_DURATION_MS;
}

#endif /* Public_h */
