//
//  SCPlayer.h
//  SCFFmpeg
//
//  Created by stan on 2024/7/8.
//  Copyright © 2024 石川. All rights reserved.
//

#ifndef SCPlayer_h
#define SCPlayer_h

#include <pthread.h>
#include <unistd.h>
#include <stdint.h>
#include <libavutil/avutil.h>
#include <libavutil/fifo.h>
#include <libavutil/time.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <time.h>
#include <sys/time.h>
#include <signal.h>

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
static inline double sc_gettime_ms(void) {
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


static int is_full_screen = 0;

enum {
  AV_SYNC_AUDIO_MASTER,
  AV_SYNC_VIDEO_MASTER,
  AV_SYNC_EXTERNAL_MASTER,
};

static int av_sync_type = AV_SYNC_AUDIO_MASTER;

// 定义一个函数指针类型
// opaque: VideoState*; userData: 调用方上下文（如 ViewController*）
typedef int (*frame_call_bacl)(AVFrame *, int, void *opaque, void *userData);


typedef struct MyPacketEle
{
    AVPacket *pkt;
} MyPacketEle;

typedef struct PacketQueue
{
    AVFifo *pkts;     // 储存元素
    int nb_packets;   // 元素的个数
    int size;         // 整个queue的大小
    int64_t duration; // 包队列累加 duration（流 time_base 刻度，非 ms）
    int abort_request;

    pthread_mutex_t mutex; // 互斥锁
    pthread_cond_t cond;   // 条件变量

} PacketQueue;

typedef struct Fram{
    AVFrame *frame;  //存储解码后的视频帧
    double pts;      //帧的呈现时刻（ms）
    double duration; //帧的持续时间（ms）
    int64_t pos;     //在pkt中的位置
    int width;       //帧的宽
    int height;      //帧的高
    int format;      //像素格式
    AVRational sar;  //帧的宽高比
}Frame;

typedef struct FrameQueue{
    Frame queue[FRAME_QUEUE_SIZE];//帧数组
    int  rindex;                  //读帧索引
    int  windex;                  //写帧索引
    int  size;                    //整个队列的大小
    int  abort;                   //队列终止标志
    pthread_mutex_t mutex; //互斥
    pthread_cond_t  cond;  //同步
}FrameQueue;



typedef struct AudioInfo{
    int wanted_nb_channels;
    /*9.初始化音频设备参数
      为音频设备设置参数
    */
    int freq;                   // 采样率
    int format;                 // 采样格式，如 AV_SAMPLE_FMT_S16
    int channels;               // 声道数
    int silence;                // 静默音
    int samples;                // 采样个数
    void *userdata;
} AudioInfo;



typedef struct VideoState{
    //文件头信息
    char *filename;
    AVFormatContext *ic;

    //音视频同步相关
    int av_sync_type;

    double          audio_clock;     //音频时钟（ms）
    double          frame_timer;     //下一帧应对齐的墙钟时刻（ms）
    double          frame_last_pts;  //上一帧视频 pts（ms）
    double          frame_last_delay;//上一帧间隔 delay（ms）

    double          video_clock;//视频时钟（ms）
    double          video_current_pts;     //当前视频帧 pts（ms）
    double          video_current_pts_time;//记下该 pts 时的墙钟（ms）

    // 音频
    int             audio_index;     //音频流的index
    AVStream        *audio_st;       //音频流
    AVCodecContext  *audio_ctx;      //音频的解码环境
    PacketQueue     audioq;          //音频的队列
    uint8_t         *audio_buf;      // 解码后到音频数据
    unsigned int    audio_buf_size;  // 当前软件缓冲字节数
    unsigned int    audio_buf_index; // 软件缓冲已交给设备的字节数
    unsigned int    audio_aq_size;   // AudioQueue 中尚未播完的字节数
    AVFrame         audio_frame;     //音频的frame
    AVPacket        audio_pkt;       //音频包
    uint8_t         *audio_pkt_data; //音频原始数据
    int             audio_pkt_size;
    struct SwrContext *audio_swr_ctx; //音频重采样
    AudioInfo       audioInfo;       //音频参数

    // 视频
    int             video_index; //视频流的index
    AVStream        *video_st;  //视频流
    AVCodecContext  *video_ctx; //视频的解码环境
    PacketQueue     videoq;     //视频包的queue
    AVPacket        video_pkt;  //视频pkt
    struct SwsContext *sws_ctx; //视频重采样
    FrameQueue      pictq;      //储存解码后的视频帧
    int width, height, xleft, ytop;//视频在SDL窗口位置和大小
    double         delay_video_time; // 刷新线程休眠时长（ms）
    double         frame_duration;//视频帧持续时间（ms）
    int            display_busy; /* 仅送显门闩：模块置 1，接入方完成显示后清 0；不参与同步 */
    int            video_rotate; /* 流元数据旋转角（度，0/90/180/270），显示时纠正 */
    int            hw_video; /* 1=VideoToolbox 硬解已启用（get_format 确认后） */
    enum AVPixelFormat hw_pix_fmt; /* 硬解目标像素格式（一般为 AV_PIX_FMT_VIDEOTOOLBOX） */
    int            hw_fallback; /* 1=VT session 失败，需重开软解并重送当前包 */
  
    //线程和退出
    pthread_t       read_tid;  //读取数据线程
    pthread_t       decode_tid;//解码线程
    pthread_t       video_loop_tid; //刷新线程
    int             has_read_tid;
    int             has_decode_tid;
    int             has_video_loop_tid;
    int             quit;
    
    //回调
    frame_call_bacl fn_call;
    void *userData; // 回调透传给上层，避免全局对象指针
    
    int out_audio_size;
    
    //音视频同步测试
    int vidoe_stop;// 1 视频暂停,0视频播放
}VideoState;

/* 同步用的帧间隔：优先用已算出的 frame_duration，否则默认 */
static inline double sc_video_frame_ms(const VideoState *is) {
    if (is && is->frame_duration > 0) {
        return is->frame_duration;
    }
    return SC_DEFAULT_FRAME_DURATION_MS;
}

int scplayer(const char *filename, frame_call_bacl fn_call, void *userData);//同步好的视频帧
/* 停止并释放一次播放（切换片源前调用） */
void scplayer_stop(VideoState *is);
/* VT 硬解失败后整路重开为软解（模拟器上常见） */
int sc_video_reopen_software(VideoState *is);


/* 公共队列接口（供音视频模块使用） */
int packet_queue_put(PacketQueue *q, AVPacket *pkt);
int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block);
Frame *frame_queue_peek_writable(FrameQueue *fq);
void frame_queue_push(FrameQueue *fq);
Frame *frame_queue_peek(FrameQueue *fq);
void fream_queue_pop(FrameQueue *fq);
double get_maste_clock(VideoState *is);

#endif /* SCPlayer_h */
