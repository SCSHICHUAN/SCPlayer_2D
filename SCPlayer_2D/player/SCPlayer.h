//
//  SCPlayer.h
//  SCFFmpeg
//
//  Created by stan on 2024/7/8.
//  Copyright © 2024 石川. All rights reserved.
//

#ifndef SCPlayer_h
#define SCPlayer_h

//#include "SCSDL.h"
#include <SDL2/SDL.h>
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

//sdl
#define FF_REFRESH_EVENT (100)
#define FF_QUIT_EVENT (100 + 1)

#define MAX_QUEUE_SIZE (5 * 1024 * 1024)
#define AUDIO_BUFFER_SIZE 1024
/*
1.00 秒 = 1000.00 毫秒
1.00 秒 = 1000000.00 微秒 用英文表示
1.00 s = 1000.00 ms
1.00 s = 1000000.00 µs
 */
#define ch_µs_to_s 1000000.0

#define VIDEO_PICTURE_QUEUE_SIZE 3
#define SAMPLE_QUEUE_SIZE 9
#define FRAME_QUEUE_SIZE FFMAX(SAMPLE_QUEUE_SIZE, VIDEO_PICTURE_QUEUE_SIZE)

static const char *input_filename;
static const char *window_title;

//static int default_width = 1080; //期望显示的宽
//static int default_height = 720; //期望显示的高
//static int screen_width  = 0;
//static int screen_height = 0;

//static SDL_Window      *win;
//static SDL_Renderer    *renderer;
static int is_full_screen = 0;
//static int screen_left = SDL_WINDOWPOS_CENTERED;
//static int screen_top = SDL_WINDOWPOS_CENTERED;




enum {
  AV_SYNC_AUDIO_MASTER,
  AV_SYNC_VIDEO_MASTER,
  AV_SYNC_EXTERNAL_MASTER,
};

static int av_sync_type = AV_SYNC_AUDIO_MASTER;

// SDL 窗口的大小
static int w_width = 720;
static int w_height = 480;

//static SDL_Window *win = NULL;
//static SDL_Renderer *renderer = NULL;

// 定义一个函数指针类型
typedef int (*frame_call_bacl)(AVFrame *, int,void *);


typedef struct MyPacketEle
{
    AVPacket *pkt;
} MyPacketEle;

typedef struct PacketQueue
{
    AVFifo *pkts;     // 储存元素
    int nb_packets;   // 元素的个数
    int size;         // 整个queue的大小
    int64_t duration; // 整个queue的时长

    SDL_mutex *mutex; // 互斥锁
    SDL_cond *cond;   // 信号量

} PacketQueue;

typedef struct Fram{
    AVFrame *frame;  //存储解码后的视频帧
    double pts;      //帧的呈现时刻
    double duration; //帧的持续时间
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
    SDL_mutex *mutex; //互斥
    SDL_cond  *cond;  //同步
}FrameQueue;



typedef struct AudioInfo{
    int wanted_nb_channels;
    /*9.初始化音频设备参数
      为音频设备设置参数
    */
    int freq;                   // 采样率
    SDL_AudioFormat format;     /// 有符号的16位
    Uint8 channels;             /**< Number of channels: 1 mono, 2 stereo */
    Uint8 silence;              // 静默音
    Uint16 samples;             // 采样个数
    Uint16 padding;             /**< Necessary for some compile environments */
    Uint32 size;                /**< Audio buffer size in bytes (calculated) */
    SDL_AudioCallback callback; /**< Callback that feeds the audio device (NULL to use SDL_QueueAudio()). */
    void *userdata;             /**< Userdata passed to callback (ignored for NULL callbacks). */
} AudioInfo;



typedef struct VideoState{
    //文件头信息
    char *filename;
    AVFormatContext *ic;

    //音视频同步相关
    int av_sync_type;

    double          audio_clock;     //音频pts
    double          frame_timer;     //已经播放完成的视频的时间
    double          frame_last_pts;  //视频最后一次的pts
    double          frame_last_delay;//视频最后一次的delay

    double          video_clock;//预测视频帧的pts 视频时间，最后的frame的pts+推算的下一帧的pts
    double          video_current_pts;     //当前播报视频帧的pts
    double          video_current_pts_time;//当前播报视频帧的pts单位秒

    // 音频
    int             audio_index;     //音频流的index
    AVStream        *audio_st;       //音频流
    AVCodecContext  *audio_ctx;      //音频的解码环境
    PacketQueue     audioq;          //音频的队列
    uint8_t         *audio_buf;      // 解码后到音频数据
    unsigned int    audio_buf_size;  // 数据包的大小
    unsigned int    audio_buf_index; // 游标
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
//    SDL_Texture     *texture;   //纹理
    FrameQueue      pictq;      //储存解码后的视频帧
    int width, height, xleft, ytop;//视频在SDL窗口位置和大小
    uint32_t   delay_video_time;
    double         frame_duration;//视频帧持续时间
  
    //线程和退出
    SDL_Thread      *read_tid;  //读取数据线程
    SDL_Thread      *decode_tid;//解码线程
    int             quit;
    
    //回调
    frame_call_bacl fn_call;
    
    int out_audio_size;
}VideoState;


int scplayer(frame_call_bacl fn_call);//同步好的视频帧
//static void sdl_event_loop(VideoState *is,int ms);

/* 公共队列接口（供音视频模块使用） */
int packet_queue_put(PacketQueue *q, AVPacket *pkt);
int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block);
Frame *frame_queue_peek_writable(FrameQueue *fq);
void frame_queue_push(FrameQueue *fq);
Frame *frame_queue_peek(FrameQueue *fq);
void fream_queue_pop(FrameQueue *fq);
void schedule_refresh(VideoState *is, int delay);
double get_maste_clock(VideoState *is);

#endif /* SCPlayer_h */
