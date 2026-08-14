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
#include "Public.h"




static int is_full_screen = 0;

enum {
  AV_SYNC_AUDIO_MASTER,
  AV_SYNC_VIDEO_MASTER,
  AV_SYNC_EXTERNAL_MASTER,
};

extern int sc_av_sync_type;
void sc_set_av_sync_type(int type);
int sc_get_av_sync_type(void);




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

// 定义一个函数指针类型
// opaque: SCPlayer*; userData: 调用方上下文（如 ViewController*）
typedef int (*Player_call_other)(AVFrame *, int, void *opaque, void *userData);

typedef struct AVFilterGraph AVFilterGraph;
typedef struct AVFilterContext AVFilterContext;

typedef struct SCPlayer{
    //文件头信息
    char *filename;
    AVFormatContext *ic;

    //音视频同步相关
    int av_sync_type;
    int realtime;

    /* 外部钟（ms）：无 speed，钉住后墙钟 1:1；音频跟它，视频跟音频 */
    double          external_pts;
    double          external_pts_drift;
    double          external_last_updated;

    double          audio_ref_clock;  // 音频播放到的时间点
    double          audio_clock;      // 上次 wrote 重置时的 pts（ms）
    double          audio_frame_pts;  // 最近解码帧 pts（ms）；wrote 时才写入 audio_clock
    /* 当前写入 AQ 的这一包 PCM：时钟 = pts + 本包内已播时长(钳在 0~本包时长) */
    double          audio_pkt_wall_ms;     /* 本包 wrote 时的墙钟（ms） */
    unsigned int    audio_pkt_bytes;      /* 本包字节数 → 本包时长 */
    int             audio_pkt_ready;      /* 已有至少一次 wrote */
    int             audio_pkt_paused;
    double          audio_pkt_elapsed_ms; /* 暂停时冻结的本包已播时长 */
    pthread_mutex_t audio_clock_mutex;
    double          audio_diff_cum;
    double          audio_diff_avg_coef;
    int             audio_diff_avg_count;
    double          audio_diff_threshold_ms;
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
    unsigned int    audio_buf_cursor; // 本帧已交给 AudioQueue 的字节数
    AVFrame         audio_frame;     //音频的frame
    AVPacket        audio_pkt;       //音频包
    uint8_t         *audio_pkt_data; //音频原始数据
    int             audio_pkt_size;
    struct SwrContext *audio_swr_ctx; //音频重采样
    /* atempo：变速不变调（EXTERNAL 快消耗时用） */
    AVFilterGraph   *audio_filter_graph;
    AVFilterContext *audio_buffersrc_ctx;
    AVFilterContext *audio_buffersink_ctx;
    double           audio_filter_tempo;
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
    
    //回调其他模块
    Player_call_other player_call_other;
    void *userData; // 回调透传给上层，避免全局对象指针
    
    int out_audio_size;
    
    //音视频同步测试
    int vidoe_stop; /* 1=视频暂停(刷新线程休眠), 0=播；暂停分支也必须查 quit，见 video_refresh_loop */
}SCPlayer;

int scplayer(const char *filename, Player_call_other player_call_other, void *userData);//同步好的视频帧
/* 停止并释放一次播放（切换片源前调用） */
void scplayer_stop(SCPlayer *scp);
/* VT 硬解失败后整路重开为软解（模拟器上常见） */
int sc_video_reopen_software(SCPlayer *scp);


/* 公共队列接口（供音视频模块使用） */
int packet_queue_put(PacketQueue *q, AVPacket *pkt);
int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block);
Frame *frame_queue_peek_writable(FrameQueue *fq);
void frame_queue_push(FrameQueue *fq);
Frame *frame_queue_peek(FrameQueue *fq);
void fream_queue_pop(FrameQueue *fq);
double get_maste_clock(SCPlayer *scp);

#endif /* SCPlayer_h */
