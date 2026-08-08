//
//  SCPlayer.c
//  SCFFmpeg
//
//  Created by stan on 2024/7/8.
//  Copyright © 2024 石川. All rights reserved.
//

#include "SCPlayer.h"
#include "SCPlayer_audio.h"
#include "SCPlayer_video.h"
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libavutil/display.h>
#include <libavcodec/codec.h>
#include <stdlib.h>
#include <math.h>

/*
 Create by stan 2024-6-30
 */

/*
 静态链接 libavcodec.a 时，若不引用 VT hwaccel 符号，链接器会丢掉
 videotoolbox.o，导致 get_format 列表里没有 AV_PIX_FMT_VIDEOTOOLBOX。
 */
extern char ff_h264_videotoolbox_hwaccel;
extern char ff_hevc_videotoolbox_hwaccel;
extern char ff_mpeg4_videotoolbox_hwaccel;
extern char ff_mpeg2_videotoolbox_hwaccel;

__attribute__((used)) static const void *const sc_force_link_videotoolbox[] = {
    &ff_h264_videotoolbox_hwaccel,
    &ff_hevc_videotoolbox_hwaccel,
    &ff_mpeg4_videotoolbox_hwaccel,
    &ff_mpeg2_videotoolbox_hwaccel,
};

/*
 一.main 主线程中 --------线程 1 主线程
 1.判断url的合法性
 2.创建win 和renderer
 
 二.调用stream_open()函数
 1.创建scp结构体
 2.创建video，audio，frame 队列
 3.创建线程“read_thread()”读包队列------线程2
 
 三.在读包线程中
 1.打开多媒体 avformat_open_input()
 2.for循环读取音视频包保存到，视频包队列，和音频包队列中
 3.调用 audio_open() 设置音频的播放的参数，和会调函数 sdl_audio_callback ----线程3
 系统创建一个线程，读队列中的数据给声卡，解码decode，解码后就直接播放
 4.调用 decode_thread () 创建视频解码线程，读取包解码decode,把解码到到Frame插入到Frame队列中，等待显示  ---线程4
 
 四.音视频同步 ---线程1中 主线程
 */


/*
 ========================自定义pkt queue========================
 */
// 初始化pkt队列
static int packet_queue_init(PacketQueue *q)
{
    memset(q, 0, sizeof(PacketQueue));
    q->pkts = av_fifo_alloc2(1, sizeof(MyPacketEle), AV_FIFO_FLAG_AUTO_GROW); // AV_FIFO_FLAG_AUTO_GROW 自动增长
    if (!q->pkts)
    {
        return AVERROR(ENOMEM);
    }
    
    if (pthread_mutex_init(&q->mutex, NULL) != 0)
    {
        return AVERROR(ENOMEM);
    }
    
    if (pthread_cond_init(&q->cond, NULL) != 0)
    {
        pthread_mutex_destroy(&q->mutex);
        return AVERROR(ENOMEM);
    }
    return 0;
}

// 清空pkt队列
static void packet_queue_flush(PacketQueue *q)
{
    MyPacketEle mypkt;
    
    pthread_mutex_lock(&q->mutex);
    while (av_fifo_read(q->pkts, &mypkt, 1) > 0)
    {
        av_packet_free(&mypkt.pkt);
    }
    q->nb_packets = 0;
    q->size = 0;
    q->duration = 0;
    
    pthread_mutex_unlock(&q->mutex);
}

// 销毁pkt队列
static void packet_queue_destroy(PacketQueue *q)
{
    packet_queue_flush(q);
    av_fifo_freep2(&q->pkts);
    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->cond);
}

// put 私有的方法
static int packet_queue_put_priv(PacketQueue *q, AVPacket *pkt)
{
    int ret;
    MyPacketEle mypkt;
    
    mypkt.pkt = pkt;
    
    ret = av_fifo_write(q->pkts, &mypkt, 1); // 把pkt添加到fifo中 保存
    if (ret < 0)
    {
        return ret;
    }
    
    q->nb_packets++;                            // 队列中包数量加1
    q->size += mypkt.pkt->size + sizeof(mypkt); // 队列的size
    q->duration += mypkt.pkt->duration;         // 队列总时长
    
    pthread_cond_signal(&q->cond); // 告诉等待线程
    
    return 0;
}

// put 把AVPacket 保存到队列中
int packet_queue_put(PacketQueue *q, AVPacket *pkt)
{
    int ret;
    AVPacket *pkt1;
    
    pkt1 = av_packet_alloc();
    if (!pkt1)
    {
        av_packet_unref(pkt);
        av_packet_unref(pkt1);
        av_log(NULL, AV_LOG_ERROR, "内存不足!\n");
    }
    // pkt的所有内容和值和引用技术给pkt1 后pkt恢复到原始状态
    av_packet_move_ref(pkt1, pkt);
    
    pthread_mutex_lock(&q->mutex);
    ret = packet_queue_put_priv(q, pkt1);
    pthread_mutex_unlock(&q->mutex);
    
    if (ret < 0)
    {
        av_packet_free(&pkt1);
    }
    
    return ret;
}

// get 获取保存AVPacket blocl表示是否阻塞
int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block)
{
    int ret;
    MyPacketEle mypkt;
    
    pthread_mutex_lock(&q->mutex);
    for (;;) {
        if (q->abort_request) {
            ret = -1;
            break;
        }
        if (av_fifo_read(q->pkts, &mypkt, 1) >= 0) {
            q->nb_packets--;
            q->size -= mypkt.pkt->size + sizeof(mypkt);
            av_packet_move_ref(pkt, mypkt.pkt); // 给出数据包
            av_packet_free(&mypkt.pkt);
            ret = 1;
            break;
        }
        else if (!block){ // 非阻塞直接返回
            ret = 0;
            break;
        } else {
            pthread_cond_wait(&q->cond, &q->mutex); // 阻塞等待
        }
    }
    pthread_mutex_unlock(&q->mutex);
    return ret;
}

static void packet_queue_abort(PacketQueue *q)
{
    pthread_mutex_lock(&q->mutex);
    q->abort_request = 1;
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->mutex);
}




/*
 自定义Frame queue
 生产线程 decode 线程、
 消费线程是 渲染线程
 */
//初始化Frame queue
static int frame_queue_init(FrameQueue *fq){
    int i;
    memset(fq,0,sizeof(FrameQueue));//所有字节设置为 0
    /*
     初始化线程标志
     */
    if(pthread_mutex_init(&fq->mutex, NULL) != 0){
        av_log(NULL,AV_LOG_FATAL,"pthread_mutex_init()\n");
        return AVERROR(ENOMEM);
    }
    if(pthread_cond_init(&fq->cond, NULL) != 0){
        av_log(NULL,AV_LOG_FATAL,"pthread_cond_init()\n");
        pthread_mutex_destroy(&fq->mutex);
        return AVERROR(ENOMEM);
    }
    /*
     初始化数组
     */
    for(i = 0; i < VIDEO_PICTURE_QUEUE_SIZE; i++)
        if(!(fq->queue[i].frame = av_frame_alloc()))
            return AVERROR(ENOMEM);
    
    return 0;
}
//销毁Frame queue
static void frame_queue_destory(FrameQueue *fq){
    
    for(int i = 0; i < VIDEO_PICTURE_QUEUE_SIZE; i++){
        Frame *vp = &fq->queue[i];
        //释放 AVFrame 结构中引用的所有数据（如内部缓冲区）
        av_frame_unref(vp->frame);
        /*
         释放 AVFrame 结构本身的内存，
         还会将 vp->frame 指针设置为 NULL，表示这个 AVFrame 已经被释放且不可再使用。
         */
        av_frame_free(&vp->frame);
        //销毁现在变量标志
        pthread_mutex_destroy(&fq->mutex);
        pthread_cond_destroy(&fq->cond);
    }
    
}
//帧队列终止
static void frame_queue_abort(FrameQueue *fq){
    pthread_mutex_lock(&fq->mutex);
    fq->abort = 1;
    pthread_cond_signal(&fq->cond);
    pthread_mutex_unlock(&fq->mutex);
}
//唤醒等待的线程
static void frame_queue_signal(FrameQueue *fq){
    /*
     唤醒等待在条件变量 fq->cond 上的一个线程。
     如果有多个线程在等待这个条件变量，那么只有一个线程会被唤醒。
     */
    pthread_mutex_lock(&fq->mutex);
    pthread_cond_signal(&fq->cond);
    pthread_mutex_unlock(&fq->mutex);
}

/*
 解码线程调用
 */
//fq_1.获取当前写视频帧位置0，当前可储存AVFrame 的Frame
Frame *frame_queue_peek_writable(FrameQueue *fq){
    pthread_mutex_lock(&fq->mutex);
    /*
     生产没有消费完，等待消费，Frame queue没有处理消费，
     因为一般在播放器中，生产>消费，不用担心消费过快
     1.这个设计没有考虑 消费>生产 ， 如果出现消费大于生产，就会黑屏因为没有视频帧，
     2.我看在ffplay中是考虑了一下这个情况 加了 frame_queue_peek_readable() 函数
     */
    while(fq->size >= VIDEO_PICTURE_QUEUE_SIZE && !fq->abort){
        pthread_cond_wait(&fq->cond,&fq->mutex);//生产过剩 等待消费 等待队列有空间插入新帧
    }
    pthread_mutex_unlock(&fq->mutex);
    
    if(fq->abort)
        return NULL;
    
    return &fq->queue[fq->windex];
}
//fq_2.视频帧已经保存到帧队列0的位置，写的index跳到1的位置
void frame_queue_push(FrameQueue *fq){
    if(++fq->windex >= VIDEO_PICTURE_QUEUE_SIZE)
        fq->windex = 0;
    pthread_mutex_lock(&fq->mutex);
    fq->size++;
    pthread_cond_signal(&fq->cond);//生产一个，发送消息给，等待消费的(本节中没有等待消费处理)
    pthread_mutex_unlock(&fq->mutex);
}
/*
 渲染线程调用
 */
//fq_3.从帧队列中0的位置，读取Frame渲染
Frame *frame_queue_peek(FrameQueue *fq){
    return &fq->queue[fq->rindex];
}
//fq_4.释放已经渲染位置的的帧，读帧的index调到1
void fream_queue_pop(FrameQueue *fq){
    Frame *vp = &fq->queue[fq->rindex];
    av_frame_unref(vp->frame);//结构中引用的所有数据
    if(++fq->rindex >= VIDEO_PICTURE_QUEUE_SIZE)
        fq->rindex = 0;
    pthread_mutex_lock(&fq->mutex);
    fq->size--;
    pthread_cond_signal(&fq->cond);//消费了一个，发出同步消息，给生产，如果生产在等待可以开始工作了
    pthread_mutex_unlock(&fq->mutex);
}

/* VideoToolbox：优先硬解；session 建失败时 FFmpeg 会再调一次，必须回退软格式并标记重开 */
static enum AVPixelFormat sc_get_hw_format(AVCodecContext *ctx,
                                           const enum AVPixelFormat *pix_fmts)
{
    const enum AVPixelFormat *p;
    enum AVPixelFormat hw_pix_fmt = AV_PIX_FMT_NONE;
    enum AVPixelFormat fallback = AV_PIX_FMT_NONE;
    SCPlayer *scp = ctx->opaque ? (SCPlayer *)ctx->opaque : NULL;
    
    if (scp) {
        hw_pix_fmt = scp->hw_pix_fmt;
    }
    
    for (p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (hw_pix_fmt != AV_PIX_FMT_NONE && *p == hw_pix_fmt) {
            if (scp) {
                scp->hw_video = 1;
                scp->hw_fallback = 0;
            }
            av_log(ctx, AV_LOG_INFO, "hw get_format: %s (HW confirmed)\n",
                   av_get_pix_fmt_name(*p));
            return *p;
        }
        if (fallback == AV_PIX_FMT_NONE && *p != AV_PIX_FMT_VIDEOTOOLBOX) {
            fallback = *p;
        }
    }
    
    if (fallback == AV_PIX_FMT_NONE && pix_fmts && pix_fmts[0] != AV_PIX_FMT_NONE) {
        fallback = pix_fmts[0];
    }
    
    /* VT 不在候选列表：说明刚 setup 失败，软格式续命不够，标记整路重开软解 */
    if (scp) {
        scp->hw_video = 0;
        scp->hw_pix_fmt = AV_PIX_FMT_NONE;
        scp->hw_fallback = 1;
    }
    ctx->get_format = NULL;
    av_buffer_unref(&ctx->hw_device_ctx);
    
    av_log(ctx, AV_LOG_WARNING,
           "hw get_format: VT setup failed, will reopen software (%s)\n",
           av_get_pix_fmt_name(fallback));
    return fallback;
}

/* 查询 codec 是否支持 VideoToolbox，并创建 hw_device_ctx */
static int sc_hw_decoder_init(AVCodecContext *avctx, const AVCodec *codec,
                              enum AVPixelFormat *out_hw_pix_fmt)
{
    AVBufferRef *device_ref = NULL;
    int i, ret;
    
    *out_hw_pix_fmt = AV_PIX_FMT_NONE;
    
    for (i = 0;; i++) {
        const AVCodecHWConfig *config = avcodec_get_hw_config(codec, i);
        if (!config) {
            av_log(avctx, AV_LOG_WARNING,
                   "decoder %s has no VideoToolbox hw_config\n", codec->name);
            return AVERROR(ENOSYS);
        }
        if ((config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
            config->device_type == AV_HWDEVICE_TYPE_VIDEOTOOLBOX) {
            *out_hw_pix_fmt = config->pix_fmt;
            av_log(avctx, AV_LOG_INFO,
                   "VideoToolbox hw_config ok, pix_fmt=%s\n",
                   av_get_pix_fmt_name(config->pix_fmt));
            break;
        }
    }
    
    ret = av_hwdevice_ctx_create(&device_ref, AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
                                 NULL, NULL, 0);
    if (ret < 0) {
        av_log(avctx, AV_LOG_WARNING,
               "av_hwdevice_ctx_create(VideoToolbox) failed: %s\n", av_err2str(ret));
        return ret;
    }
    
    avctx->hw_device_ctx = av_buffer_ref(device_ref);
    av_buffer_unref(&device_ref);
    if (!avctx->hw_device_ctx) {
        return AVERROR(ENOMEM);
    }
    
    avctx->get_format = sc_get_hw_format;
    return 0;
}

static void sc_hw_decoder_uninit(AVCodecContext *avctx)
{
    if (!avctx) {
        return;
    }
    avctx->get_format = NULL;
    av_buffer_unref(&avctx->hw_device_ctx);
}

/* VT session 失败后：丢弃硬解上下文，全新软解 open（保留 opaque 以外的绑定由调用方换指针） */
int sc_video_reopen_software(SCPlayer *scp)
{
    AVCodecContext *old_ctx;
    AVCodecContext *avctx = NULL;
    const AVCodec *codec;
    int ret;
    
    if (!scp || !scp->video_st) {
        return AVERROR(EINVAL);
    }
    
    old_ctx = scp->video_ctx;
    codec = avcodec_find_decoder(scp->video_st->codecpar->codec_id);
    if (!codec) {
        return AVERROR_DECODER_NOT_FOUND;
    }
    
    avctx = avcodec_alloc_context3(codec);
    if (!avctx) {
        return AVERROR(ENOMEM);
    }
    
    ret = avcodec_parameters_to_context(avctx, scp->video_st->codecpar);
    if (ret < 0) {
        avcodec_free_context(&avctx);
        return ret;
    }
    
    avctx->opaque = scp;
    /* 纯软解，不再挂 get_format / hw_device_ctx */
    ret = avcodec_open2(avctx, codec, NULL);
    if (ret < 0) {
        avcodec_free_context(&avctx);
        return ret;
    }
    
    if (old_ctx) {
        sc_hw_decoder_uninit(old_ctx);
        avcodec_free_context(&old_ctx);
    }
    
    scp->video_ctx = avctx;
    scp->hw_video = 0;
    scp->hw_pix_fmt = AV_PIX_FMT_NONE;
    scp->hw_fallback = 0;
    if (scp->sws_ctx) {
        sws_freeContext(scp->sws_ctx);
        scp->sws_ctx = NULL;
    }
    
    av_log(NULL, AV_LOG_INFO, "video decoder: software (reopened after VT failure)\n");
    return 0;
}

/* 读取流旋转元数据（度），对齐 ffplay：dscpplaymatrix / rotate tag */
static int sc_get_stream_rotation(AVStream *st)
{
    double theta = 0;
    size_t size = 0;
    uint8_t *dscpplaymatrix = NULL;
    AVDictionaryEntry *entry = NULL;
    int deg;
    
    if (!st) {
        return 0;
    }
    dscpplaymatrix = av_stream_get_side_data(st, AV_PKT_DATA_DISPLAYMATRIX, &size);
    if (dscpplaymatrix && size >= 9 * sizeof(int32_t)) {
        theta = -av_display_rotation_get((const int32_t *)dscpplaymatrix);
    } else {
        entry = av_dict_get(st->metadata, "rotate", NULL, 0);
        if (entry && entry->value && entry->value[0]) {
            theta = atof(entry->value);
        }
    }
    deg = (int)llround(theta / 90.0) * 90;
    deg = ((deg % 360) + 360) % 360;
    return deg;
}

int stream_component_open(SCPlayer *scp,int stream_index){
    
    int ret =-1;
    
    AVFormatContext *ic = scp->ic;
    AVCodecContext *avctx = NULL;
    const AVCodec *codec = NULL;
    
    int sample_rate;
    AVChannelLayout ch_layout = {0, };
    
    AVStream *st = NULL;
    int codec_id;
    
    
    if(stream_index < 0 || stream_index >= ic->nb_streams){
        return -1;
    }
    
    /*
     视频解码初始化
     */
    st = ic->streams[stream_index];
    codec_id = st->codecpar->codec_id;
    // dc_1. 根据流中的codec_id, 获得解码器
    codec = avcodec_find_decoder(codec_id);
    if (!codec) {
        av_log(NULL, AV_LOG_ERROR, "Could not find Codec");
        goto __ERROR;
    }
    // dc_2. 创建解码器上下文
    avctx = avcodec_alloc_context3(codec);
    if (!avctx){
        av_log(NULL, AV_LOG_ERROR, "内存不足\n");
        goto __ERROR;
    }
    // dc_3. 从视频流中拷贝解码器参数到解码器上文中
    ret = avcodec_parameters_to_context(avctx,st->codecpar);
    if (ret < 0){
        av_log(avctx, AV_LOG_ERROR, "不能拷贝解码参数到视频解码环境中!\n");
        goto __ERROR;
    }
    
    /* 视频：优先 VideoToolbox 硬解；真正 HW 要等 get_format 确认 */
    if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
        scp->hw_video = 0;
        scp->hw_pix_fmt = AV_PIX_FMT_NONE;
        scp->hw_fallback = 0;
        avctx->opaque = scp;
        
        if (sc_hw_decoder_init(avctx, codec, &scp->hw_pix_fmt) == 0) {
            ret = avcodec_open2(avctx, codec, NULL);
            if (ret < 0) {
                av_log(avctx, AV_LOG_WARNING,
                       "硬解 open 失败 (%s)，回退软解\n", av_err2str(ret));
                sc_hw_decoder_uninit(avctx);
                scp->hw_pix_fmt = AV_PIX_FMT_NONE;
                ret = avcodec_open2(avctx, codec, NULL);
            } else if (avctx->hw_device_ctx != NULL) {
                /* 此时往往尚未 get_format；模拟器常在首帧才暴露 VT 失败 */
                av_log(NULL, AV_LOG_INFO,
                       "video decoder: VideoToolbox pending (device ok)\n");
            } else {
                av_log(NULL, AV_LOG_WARNING,
                       "open ok but hw_device_ctx cleared, soft\n");
                sc_hw_decoder_uninit(avctx);
                scp->hw_pix_fmt = AV_PIX_FMT_NONE;
            }
        } else {
            sc_hw_decoder_uninit(avctx);
            scp->hw_pix_fmt = AV_PIX_FMT_NONE;
            ret = avcodec_open2(avctx, codec, NULL);
        }
        
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "打开视频解码器失败: %s \n", av_err2str(ret));
            goto __ERROR;
        }
        if (!scp->hw_video && !avctx->hw_device_ctx) {
            av_log(NULL, AV_LOG_INFO, "video decoder: software\n");
        }
    } else {
        // dc_4. 绑定解码器和上下文（音频等）
        ret = avcodec_open2(avctx, codec, NULL);
        if (ret < 0){
            av_log(avctx, AV_LOG_ERROR, "打开解码器失败: %s \n", av_err2str(ret));
            goto __ERROR;
        }
    }
    
    switch (avctx->codec_type){
        case AVMEDIA_TYPE_AUDIO:
            /*
             scp音频结构体赋值
             */
            sample_rate = avctx->sample_rate;
            ret = av_channel_layout_copy(&ch_layout,&avctx->ch_layout);//拷贝音频设备参数
            if(ret < 0){
                goto __ERROR;
            }
            ret = audio_wanted_spec(scp,&ch_layout,sample_rate);
            if(ret < 0){
                av_log(NULL,AV_LOG_ERROR,"不能打开音频设备!\n");
                // goto __ERROR;
            }
            scp->audio_buf_size = 0;
            scp->audio_buf_cursor = 0;
            scp->audio_clock = NAN;
            scp->audio_write_pts = NAN;
            scp->audio_st = st;
            scp->audio_index = stream_index;
            scp->audio_ctx = avctx;
            audio_aq_interp_start(scp);
            
            //打开音频设备开始播放  SDL_PauseAudio(0);
            scp->player_call_other(NULL,0,scp,scp->userData);
            break;
        case AVMEDIA_TYPE_VIDEO:
            /*
             scp视频结构体赋值
             */
            scp->video_index = stream_index;
            scp->video_st = st;
            scp->video_ctx = avctx;
            scp->frame_timer = av_gettime_ms();//第一帧视频播放的墙钟时刻（ms）
            /* 帧间隔由流 fps 计算；拿不到则 SC_DEFAULT_FRAME_DURATION_MS(40ms≈25fps) */
            scp->frame_duration = sc_frame_duration_from_stream(scp->ic, st);
            scp->frame_last_delay = scp->frame_duration;
            scp->display_busy = 0;
            scp->video_rotate = sc_get_stream_rotation(st);
            scp->video_current_pts_time = av_gettime_ms();//记下 pts 时的墙钟（ms）
            av_log(NULL, AV_LOG_INFO, "video frame_duration=%.3f ms rotate=%d\n",
                   scp->frame_duration, scp->video_rotate);
            
            if(pthread_create(&scp->decode_tid, NULL, video_decode_thread, scp) != 0){
                av_log(NULL,AV_LOG_FATAL,"pthread_create(video_decode_thread)\n");
                goto __ERROR;
            }
            scp->has_decode_tid = 1;
            break;
        case AVMEDIA_TYPE_UNKNOWN:
            av_log(avctx,AV_LOG_ERROR,"Other media type unknow - media_type = %d\n",avctx->codec_type);
            break;
        default:
            av_log(avctx,AV_LOG_INFO,"Other media type - media_type = %d\n",avctx->codec_type);
            break;
    }
    
    
    ret = 0;
    goto __END;
__ERROR:
    if(avctx){
        avcodec_free_context(&avctx);
    }
__END:
    return ret;
}

//=========================== 网络中读取音视频包保存到音视频包队列中 ===========================
void *read_thread(void *arg){
    //    Uint32 pixformat;
    int ret = -1;
    int video_index  = -1;
    int audio_index  = -1;
    
    SCPlayer *scp = (SCPlayer*)arg;
    AVFormatContext *ic = NULL;
    AVPacket *pkt = NULL;
    
    pkt = av_packet_alloc();
    if(!pkt){
        av_log(NULL, AV_LOG_FATAL, "NO MEMORY!\n");
        goto __ERROR;
    }
    
    //1. Open media file
    if((ret = avformat_open_input(&ic, scp->filename, NULL, NULL)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Could not open file: %s, %d(%s)\n", scp->filename, ret, av_err2str(ret));
        goto __ERROR; // Couldn't open file
    }
    scp->ic = ic;
    
    //2. extract media info
    if(avformat_find_stream_info(ic, NULL) < 0) {
        av_log(NULL, AV_LOG_FATAL, "Couldn't find stream information\n");
        goto __ERROR;
    }
    
    //3. Find the first audio and video stream
    for(int i = 0; i < ic->nb_streams; i++) {
        AVStream *st = ic->streams[i];
        enum AVMediaType type = st->codecpar->codec_type;
        if(type == AVMEDIA_TYPE_VIDEO && video_index < 0) {
            video_index=i;
        }
        if(type == AVMEDIA_TYPE_AUDIO && audio_index < 0) {
            audio_index=i;
        }
        //find video and audio
        if(video_index > -1 && audio_index > -1) {
            break;
        }
    }
    
    if(video_index < 0 || audio_index < 0) {
        av_log(NULL, AV_LOG_ERROR, "多媒体文件必须包含视频和音频!\n");
        goto __ERROR;
    }
    
    //音视频编解码器初始化
    if(audio_index >= 0){
        stream_component_open(scp,audio_index);
    }
    if(video_index >= 0 ){
        //打开视频流
        stream_component_open(scp, video_index);
    }
    
    //读取多媒体包保存在pkt queue中
    for(;;){
        
        if(scp->quit){
            ret = -1;
            goto __ERROR;
        }
        
        //没有消费完循环等待10ms，queue满了
        if(scp->audioq.size > MAX_QUEUE_SIZE ){
            sc_delay_ms(10);
            continue;
        }
//        if(scp->videoq.size > MAX_QUEUE_SIZE){
//            sc_delay_ms(10);
//            continue;
//        }
        //从上下文中读取包
        ret = av_read_frame(scp->ic,pkt);
        if(ret < 0){
            if(scp->ic->pb->error == 0){//no error; wait for user input
                /*
                 如果还没有读取到包，等100毫秒在读
                 */
                sc_delay_ms(100);
                continue;
            }else{
                break;
            }
        }
        
        //pkt 保存到 pkt queue中
        if(pkt->stream_index == scp->video_index){
            packet_queue_put(&scp->videoq,pkt); //视频包保存到视频队列
        }else if(pkt->stream_index == scp->audio_index){
            packet_queue_put(&scp->audioq,pkt); //音频包保存到音频队列
        }else{
            av_packet_unref(pkt);              //取他类型的包丢弃
        }
    }
    
    /* all done - wait for it 如果读取完了 等待100ms*/
    while (!scp->quit){
        sc_delay_ms(100);
    }
    
    
__ERROR:
    if(pkt){
        av_packet_free(&pkt);
    }
    if(ret !=0 ){
    }
    return (void *)(intptr_t)ret;
}

static void stream_component_close(SCPlayer *scp, int stream_index){
    AVFormatContext *ic = scp->ic;
    AVCodecParameters *codecpar;
    
    if (stream_index < 0 || stream_index >= ic->nb_streams)
        return;
    codecpar = ic->streams[stream_index]->codecpar;
    
    switch (codecpar->codec_type) {
        case AVMEDIA_TYPE_AUDIO:
            //      SDL_CloseAudio();
            audio_aq_interp_stop(scp);
            swr_free(&scp->audio_swr_ctx);
            av_freep(&scp->audio_buf);
            scp->audio_buf = NULL;

            break;
        case AVMEDIA_TYPE_VIDEO:
            frame_queue_abort(&scp->pictq);
            frame_queue_signal(&scp->pictq); //可以确保所有等待的线程都被唤醒
            if(scp->has_decode_tid){
                pthread_join(scp->decode_tid, NULL);
                scp->has_decode_tid = 0;
            }
            if (scp->sws_ctx) {
                sws_freeContext(scp->sws_ctx);
                scp->sws_ctx = NULL;
            }
            scp->hw_video = 0;
            break;
        default:
            break;
    }
}

static void stream_close(SCPlayer *scp){
    if(scp->has_read_tid){
        pthread_join(scp->read_tid, NULL);
        scp->has_read_tid = 0;
    }
    
    /* close each stream */
    if (scp->audio_index >= 0)
        stream_component_close(scp, scp->audio_index);
    if (scp->video_index >= 0)
        stream_component_close(scp, scp->video_index);
    
    avformat_close_input(&scp->ic);
    packet_queue_destroy(&scp->videoq);
    packet_queue_destroy(&scp->audioq);
    frame_queue_destory(&scp->pictq);
    av_free(scp->filename);
    av_free(scp);
}

static SCPlayer *stream_open(const char* filename){
    
    SCPlayer *scp;
    scp = av_mallocz(sizeof(SCPlayer));
    if(!scp){
        av_log(NULL,AV_LOG_FATAL,"内存不足！\n");
        return NULL;
    }
    
    scp->audio_index = scp->video_index =-1;
    scp->filename = av_strdup(filename);//源字符串的内容复制到新分配的内存区域，并返回指向该区域的指针
    if(!scp->filename){
        goto __ERROR;
    }
    
    scp->ytop = 0;
    scp->xleft = 0;
    
    //初始化packet queue
    if(packet_queue_init(&scp->videoq) < 0 || packet_queue_init(&scp->audioq) < 0){
        goto __ERROR;
    }
    /*
     初始化video frame queue 用于保存解码后的视频帧，
     ffplay中同时有音频的帧的queue这里为了简单,没有音频的
     */
    if(frame_queue_init(&scp->pictq) < 0){
        goto __ERROR;
    }
    
    scp->av_sync_type = av_sync_type;
    if(pthread_create(&scp->read_tid, NULL, read_thread, scp) != 0){
        av_log(NULL,AV_LOG_FATAL,"pthread_create(read_thread)\n");
        goto __ERROR;
    }
    scp->has_read_tid = 1;
    
    return scp;
__ERROR:
    stream_close(scp);
    return NULL;
}
//系统时间 / 外部时钟（ms）
double get_external_clock(void){
    return av_gettime_ms();
}

//当前音频播放的时刻
double get_maste_clock(SCPlayer *scp){
    if(scp->av_sync_type == AV_SYNC_AUDIO_MASTER){
        return get_audio_clock(scp);
    } else if(scp->av_sync_type == AV_SYNC_VIDEO_MASTER){
        return get_video_clock(scp);
    } else {
        return get_external_clock();
    }
}

static void do_exit(SCPlayer *scp){
    if(scp){
        stream_close(scp);
    }
    av_log(NULL,AV_LOG_QUIET,"%s","");
}


/*
 刷新线程主循环。
 坑：vidoe_stop==1 时若只 sleep/continue 而不查 quit，
 scplayer_stop → pthread_join(本线程) 会永远等 → 暂停后再选片主线程卡死。
 因此每轮必须先看 quit。
 */
static void video_refresh_loop(SCPlayer *scp){
    for(;;){
        if (scp->quit) {
            break;
        }
        if(scp->vidoe_stop == 1){
            sc_delay_ms(10); /* 视频暂停：让出 CPU，音频可继续 */
            continue;
        }
        video_refresh_timer(scp);
        if (scp->quit) {
            break;
        }
        sc_delay_ms(scp->delay_video_time);// 控制刷新节奏（同步仍靠 pts vs 音频时钟）
    }
}


void *video_loop(void *arg){
    SCPlayer *scp = (SCPlayer*)arg;
    video_refresh_loop(scp);
    return NULL;
}

/*
 1. 判断输入参数
 2. 初始化SDL，并创建窗口和Render
 2.1
 2.2 creat window from SDL
 3. 打开多媒体文件，并获得流信息
 4. 查找最好的视频流
 5. 根据流中的codec_id, 获得解码器
 6. 创建解码器上下文
 7. 从视频流中拷贝解码器参数到解码器上文中
 8. 绑定解码器上下文
 9. 根据视频的宽/高创建纹理
 10. 从多媒体文件中读取数据，进行解码
 11. 对解码后的视频帧进行渲染
 12. 处理SDL事件
 13. 收尾，释放资源
 */

int scplayer(const char *filename, Player_call_other player_call_other, void *userData){
    
    SCPlayer *scp;
    static int network_inited = 0;
    
    av_log_set_level(AV_LOG_INFO);
    if (!network_inited) {
        avformat_network_init();
        network_inited = 1;
    }
    
    if(!filename || !filename[0]){
        av_log(NULL,AV_LOG_FATAL,"filename scp empty\n");
        return -1;
    }
    
    scp = stream_open(filename);
    if(!scp){
        av_log(NULL,AV_LOG_FATAL,"初始化SCPlayer失败\n");
        do_exit(NULL);
        return -1;
    }
    scp->player_call_other = player_call_other;
    scp->userData = userData;
    
    
    if(pthread_create(&scp->video_loop_tid, NULL, video_loop, scp) == 0){
        scp->vidoe_stop = 0;
        scp->has_video_loop_tid = 1;
    } else {
        av_log(NULL,AV_LOG_FATAL,"pthread_create(video_loop)\n");
        do_exit(scp);
        return -1;
    }
    
    return 0;
}

/* 停播/换片：置 quit、打断队列，再 join 刷新线程（该线程在暂停分支也必须响应 quit） */
void scplayer_stop(SCPlayer *scp)
{
    if (!scp) {
        return;
    }
    scp->quit = 1;
    packet_queue_abort(&scp->videoq);
    packet_queue_abort(&scp->audioq);
    frame_queue_abort(&scp->pictq);
    frame_queue_signal(&scp->pictq);
    
    if (scp->has_video_loop_tid) {
        pthread_join(scp->video_loop_tid, NULL);
        scp->has_video_loop_tid = 0;
    }
    stream_close(scp);
}

/*
 
 “.”和“->”的区别
 
 如果是指针需要用“->”
 
 struct Person {
 int age;
 };
 
 struct Person p1;
 p1.age = 25;
 
 struct Person *p2;
 p2 = (struct Person*) malloc(sizeof(struct Person));
 p2->age = 32;
 
 */

