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

/*
Create by stan 2024-6-30
*/

/*
一.main 主线程中 --------线程 1 主线程
  1.判断url的合法性
  2.创建win 和renderer

二.调用stream_open()函数
  1.创建is结构体
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
  自定义pkt queue
 */
// 初始化队列
static int packet_queue_init(PacketQueue *q)
{
    memset(q, 0, sizeof(PacketQueue));
    q->pkts = av_fifo_alloc2(1, sizeof(MyPacketEle), AV_FIFO_FLAG_AUTO_GROW); // AV_FIFO_FLAG_AUTO_GROW 自动增长
    if (!q->pkts)
    {
        return AVERROR(ENOMEM);
    }

    q->mutex = SDL_CreateMutex();
    if (!q->mutex)
    {
        return AVERROR(ENOMEM);
    }

    q->cond = SDL_CreateCond();
    if (!q->cond)
    {
        return AVERROR(ENOMEM);
    }
    return 0;
}
// 清空队列
static void packet_queue_flush(PacketQueue *q)
{
    MyPacketEle mypkt;

    SDL_LockMutex(q->mutex);
    while (av_fifo_read(q->pkts, &mypkt, 1) > 0)
    {
        av_packet_free(&mypkt.pkt);
    }
    q->nb_packets = 0;
    q->size = 0;
    q->duration = 0;

    SDL_UnlockMutex(q->mutex);
}
// 销毁队列
static void packet_queue_destroy(PacketQueue *q)
{
    packet_queue_flush(q);
    av_fifo_freep2(&q->pkts);
    SDL_DestroyMutex(q->mutex);
    SDL_DestroyCond(q->cond);
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

    SDL_CondSignal(q->cond); // 告诉等待线程

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

    SDL_LockMutex(q->mutex);
    ret = packet_queue_put_priv(q, pkt1);
    SDL_UnlockMutex(q->mutex);

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

    SDL_LockMutex(q->mutex);
    for (;;) {
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
            SDL_CondWait(q->cond, q->mutex); // 阻塞等待
        }
    }
    SDL_UnlockMutex(q->mutex);
    return ret;
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
    if(!(fq->mutex = SDL_CreateMutex())){
        av_log(NULL,AV_LOG_FATAL,"SDL_CreateMutex()\n");
        return AVERROR(ENOMEM);
    }
    if(!(fq->cond = SDL_CreateCond())){
        av_log(NULL,AV_LOG_FATAL,"SDL_CreateCond()\n");
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
       SDL_DestroyMutex(fq->mutex);
       SDL_DestroyCond(fq->cond);
    }
        
}
//帧队列终止
static void frame_queue_abort(FrameQueue *fq){
    SDL_LockMutex(fq->mutex);
    fq->abort = 1;
    SDL_CondSignal(fq->cond);
    SDL_UnlockMutex(fq->mutex);
}
//唤醒等待的线程
static void frame_queue_signal(FrameQueue *fq){
    /*
    唤醒等待在条件变量 fq->cond 上的一个线程。
    如果有多个线程在等待这个条件变量，那么只有一个线程会被唤醒。
    */
    SDL_LockMutex(fq->mutex);
    SDL_CondSignal(fq->cond);
    SDL_UnlockMutex(fq->mutex);
}

/*
  解码线程调用
*/
//fq_1.获取当前写视频帧位置0，当前可储存AVFrame 的Frame
Frame *frame_queue_peek_writable(FrameQueue *fq){
    SDL_LockMutex(fq->mutex);
    /*
    生产没有消费完，等待消费，Frame queue没有处理消费，
    因为一般在播放器中，生产>消费，不用担心消费过快
    1.这个设计没有考虑 消费>生产 ， 如果出现消费大于生产，就会黑屏因为没有视频帧，
    2.我看在ffplay中是考虑了一下这个情况 加了 frame_queue_peek_readable() 函数
    */
   while(fq->size >= VIDEO_PICTURE_QUEUE_SIZE && !fq->abort){
        SDL_CondWait(fq->cond,fq->mutex);//生产过剩 等待消费 等待队列有空间插入新帧
   }
   SDL_UnlockMutex(fq->mutex);

   if(fq->abort)
      return NULL;

    return &fq->queue[fq->windex];
}
//fq_2.视频帧已经保存到帧队列0的位置，写的index跳到1的位置
void frame_queue_push(FrameQueue *fq){
    if(++fq->windex >= VIDEO_PICTURE_QUEUE_SIZE)
        fq->windex = 0;
    SDL_LockMutex(fq->mutex);
    fq->size++;
    SDL_CondSignal(fq->cond);//生产一个，发送消息给，等待消费的(本节中没有等待消费处理)
    SDL_UnlockMutex(fq->mutex);
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
    SDL_LockMutex(fq->mutex);
    fq->size--;
    SDL_CondSignal(fq->cond);//消费了一个，发出同步消息，给生产，如果生产在等待可以开始工作了
    SDL_UnlockMutex(fq->mutex);
}

int stream_component_open(VideoState *is,int stream_index){
    
    int ret =-1;

    AVFormatContext *ic = is->ic;
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
    // dc_4. 绑定解码器和上下文
    ret = avcodec_open2(avctx, codec, NULL);
    if (ret < 0){
        av_log(avctx, AV_LOG_ERROR, "打开视频解码器失败: %s \n", av_err2str(ret));
        goto __ERROR;
    }

    switch (avctx->codec_type){
    case AVMEDIA_TYPE_AUDIO:
         sample_rate = avctx->sample_rate;
         ret = av_channel_layout_copy(&ch_layout,&avctx->ch_layout);//拷贝音频设备参数
         if(ret < 0){
            goto __ERROR;
         }
         ret = audio_open(is,&ch_layout,sample_rate);
         if(ret < 0){
            av_log(NULL,AV_LOG_ERROR,"不能打开音频设备!\n");
           // goto __ERROR;
         }

         is->audio_buf_size = 0;
         is->audio_buf_index = 0;
         is->audio_st = st;
         is->audio_index = stream_index;
         is->audio_ctx = avctx;

         //开始播放音频
//         SDL_PauseAudio(0);
        is->fn_call(NULL,0,is,is->userData);
        break;
    case AVMEDIA_TYPE_VIDEO:
        is->video_index = stream_index;
        is->video_st = st;
        is->video_ctx = avctx;

        is->frame_timer = (double)av_gettime() / ch_µs_to_s;//第一帧视频播放的时刻，加一个delay就是第二帧，加第二哥delay就是下一帧，以此类推
        is->frame_last_delay = 40e-3;//上一次渲染delay时间
        is->video_current_pts_time = av_gettime();//当前pts的系统时间

        is->decode_tid = SDL_CreateThread(video_decode_thread,"video_decode_thread",is);
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
int read_thread(void *arg){
//    Uint32 pixformat;
    int ret = -1;
    int video_index  = -1;
    int audio_index  = -1;

    VideoState *is = (VideoState*)arg;
    AVFormatContext *ic = NULL;
    AVPacket *pkt = NULL;

    pkt = av_packet_alloc();
    if(!pkt){
        av_log(NULL, AV_LOG_FATAL, "NO MEMORY!\n");
        goto __ERROR;
    }
    
    //1. Open media file
  if((ret = avformat_open_input(&ic, is->filename, NULL, NULL)) < 0) {
    av_log(NULL, AV_LOG_ERROR, "Could not open file: %s, %d(%s)\n", is->filename, ret, av_err2str(ret));
    goto __ERROR; // Couldn't open file
  }
  is->ic = ic;
  
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
    stream_component_open(is,audio_index);
  }
  if(video_index >= 0 ){
     //如果显示帧的显示区域大于期望的区域，就等于期望的区域
//     if (codecpar->width){
//       if(codecpar->width <= default_width && codecpar->height <= default_height){
//          set_default_window_size(codecpar->width, codecpar->height, sar);
//       }else{
//          set_default_window_size(default_width, default_height, sar);
//       }
//    }else{
//      set_default_window_size(default_width, default_height, sar);
//    }

     //打开视频流
    stream_component_open(is, video_index);
   }

  //读取多媒体包保存在pkt queue中
  for(;;){
       
        if(is->quit){
            ret = -1;
            goto __ERROR;
        }

        //没有消费完循环等待10ms，queue满了
//        if(is->audioq.size > MAX_QUEUE_SIZE ||
//           is->videoq.size > MAX_QUEUE_SIZE){
//            SDL_Delay(10);
//            continue;
//           }
//              if(is->videoq.size > MAX_QUEUE_SIZE){
//                  SDL_Delay(10);
//                  continue;
//                 }
        //从上下文中读取包
        ret = av_read_frame(is->ic,pkt);
        if(ret < 0){
            if(is->ic->pb->error == 0){//no error; wait for user input
                /*
                如果还没有读取到包，等100毫秒在读
                */
                SDL_Delay(100);
                continue;
            }else{
                break;
            }
        }

        //pkt 保存到 pkt queue中
        if(pkt->stream_index == is->video_index){
            packet_queue_put(&is->videoq,pkt); //视频包保存到视频队列
        }else if(pkt->stream_index == is->audio_index){
            packet_queue_put(&is->audioq,pkt); //音频包保存到音频队列
        }else{
            av_packet_unref(pkt);              //取他类型的包丢弃
        }
    }

  /* all done - wait for it 如果读取完了 等待100ms*/
   while (!is->quit){
        SDL_Delay(100);
    }


__ERROR:
    if(pkt){
        av_packet_free(&pkt);
    }
    if(ret !=0 ){
//    SDL_Event event;
//    event.type = FF_QUIT_EVENT;
//    event.user.data1 = is;
//    SDL_PushEvent(&event);
  }

  return ret;
}

static void stream_component_close(VideoState *is, int stream_index){
  AVFormatContext *ic = is->ic;
  AVCodecParameters *codecpar;

  if (stream_index < 0 || stream_index >= ic->nb_streams)
      return;
  codecpar = ic->streams[stream_index]->codecpar;

  switch (codecpar->codec_type) {
  case AVMEDIA_TYPE_AUDIO:
//      SDL_CloseAudio();
      swr_free(&is->audio_swr_ctx);
      av_freep(&is->audio_buf);
      is->audio_buf = NULL;

      break;
  case AVMEDIA_TYPE_VIDEO:
    frame_queue_abort(&is->pictq);
    frame_queue_signal(&is->pictq); //可以确保所有等待的线程都被唤醒
    SDL_WaitThread(is->decode_tid, NULL);
    is->decode_tid = NULL;
      break;
  default:
      break;
  }
}

static void stream_close(VideoState *is){
    SDL_WaitThread(is->read_tid, NULL);

    /* close each stream */
    if (is->audio_index >= 0)
        stream_component_close(is, is->audio_index);
    if (is->video_index >= 0)
        stream_component_close(is, is->video_index);

    avformat_close_input(&is->ic);

    packet_queue_destroy(&is->videoq);
    packet_queue_destroy(&is->audioq);

    frame_queue_destory(&is->pictq);

    av_free(is->filename);
//    if(is->texture)
//        SDL_DestroyTexture(is->texture);
    av_free(is);
}
//添加定时事件
//    SDL_Event event;
//    event.type = FF_REFRESH_EVENT;
//    event.user.data1 = opaque;
//    SDL_PushEvent(&event);
void schedule_refresh(VideoState *is,int delay){
//    SDL_AddTimer(delay,sdl_refresh_timer_cb,is);
}

static VideoState *stream_open(const char* filename){

    VideoState *is;
    is = av_mallocz(sizeof(VideoState));
    if(!is){
        av_log(NULL,AV_LOG_FATAL,"内存不足！\n");
        return NULL;
    }

    is->audio_index = is->video_index =-1;
    is->filename = av_strdup(filename);//源字符串的内容复制到新分配的内存区域，并返回指向该区域的指针
    if(!is->filename){
        goto __ERROR;
    }

    is->ytop = 0;
    is->xleft = 0;
    
    //初始化packet queue
    if(packet_queue_init(&is->videoq) < 0 || packet_queue_init(&is->audioq) < 0){
        goto __ERROR;
    }
    /*
    初始化video frame queue 用于保存解码后的视频帧，
    ffplay中同时有音频的帧的queue这里为了简单,没有音频的
    */
   if(frame_queue_init(&is->pictq) < 0){
     goto __ERROR;
   }

   is->av_sync_type = av_sync_type;
   is->read_tid = SDL_CreateThread(read_thread,"read_thread",is);
   if(!is->read_tid){
      av_log(NULL,AV_LOG_FATAL,"SDL_CreateThread()\n");
      goto __ERROR;
   }

   schedule_refresh(is,40);//开始刷新视频帧 开是40ms一次
   
   return is;
__ERROR:
  stream_close(is);
  return NULL;
}
//系统时间 外界对其时间
double get_external_clock(void){
    return av_gettime()/ch_µs_to_s;
}

double get_maste_clock(VideoState *is){
    if(is->av_sync_type == AV_SYNC_AUDIO_MASTER){
        return get_audio_clock(is);
    } else if(is->av_sync_type == AV_SYNC_VIDEO_MASTER){
        return get_video_clock(is);
    } else {
        return get_external_clock();
    }
}

static void do_exit(VideoState *is){
    if(is){
        stream_close(is);
    }
//    if(renderer)
//       SDL_DestroyRenderer(renderer);
//    if(win)
//       SDL_DestroyWindow(win);
//    SDL_Quit();
    av_log(NULL,AV_LOG_QUIET,"%s","");
}


static void sdl_event_loop(VideoState *is){
//    SDL_Event event;
//    for(;;){
//        SDL_WaitEvent(&event);
//        switch(event.type){
//            case FF_QUIT_EVENT:
//            case SDL_QUIT:
//            is->quit = 1;
//            do_exit(is);
//            break;
//            case FF_REFRESH_EVENT:
//            video_refresh_timer(event.user.data1);
//            break;
//            default:
//            break;
//        }
//    }
    for(;;){
        video_refresh_timer(is);
        SDL_Delay(is->delay_video_time);
    }
    
}


int video_loop(void *arg){
    VideoState *is = (VideoState*)arg;
    sdl_event_loop(is);
    return 0;
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

int scplayer(frame_call_bacl fn_call, void *userData){

    VideoState *is;

    av_log_set_level(AV_LOG_INFO);
    input_filename = "/Users/stan/Desktop/SCPlayer_2D/ffop.mp4";


    is = stream_open(input_filename);
    if(!is){
        av_log(NULL,AV_LOG_FATAL,"初始化VideoState失败\n");
        do_exit(NULL);
        return -1;
    }
    is->fn_call = fn_call;
    is->userData = userData;

    SDL_CreateThread(video_loop,"video_loop",is);
    return 0;
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

