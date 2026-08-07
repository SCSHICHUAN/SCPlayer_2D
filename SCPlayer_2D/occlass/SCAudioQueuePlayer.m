//
//  SCAudioQueuePlayer.m
//  SCFFmpeg
//
//  Created by stan on 2024/7/11.
//  Copyright © 2024 石川. All rights reserved.
//

#import "SCAudioQueuePlayer.h"
#import <AudioToolbox/AudioToolbox.h>
#import "FFAudioInformation.h"
#include <pthread.h>
#include "SCPlayer.h"
#include "SCPlayer_audio.h"

#define NUM_BUFFERS 3
#define MAX_BUFFER_COUNT 3
/*
 最小是音频帧的大小  =  一个音频帧采样个数 x （nb_channels）音频通道数 x 位深
      4096(byte)  =   1024 x 2 x 2(byte)
  预留更大缓冲，避免 resample 后一帧超过 4096 导致 memcpy 越界
 */
#define BUFFER_SIZE (4096 * 4)


@implementation SCAudioQueuePlayer{
    AudioQueueRef audioQueue;
    struct FFAudioInformation audioInformation;
    CFMutableArrayRef buffers;
    AVStream *stream;
    
    
    AVFormatContext *formatContext;
    dispatch_queue_t decode_dscppatch_queue;
    dispatch_queue_t audio_play_dscppatch_queue;
    dispatch_queue_t video_render_dscppatch_queue;
    AVPacket *packet;
    /// lock shared variate
    pthread_mutex_t mutex;
    double video_clock;
    double tolerance_scope;
    double audio_clock;
    
    AudioQueueBufferRef audioQueueBuffers[NUM_BUFFERS];
    FILE *pcmFile;
    SCPlayer *scp;
}
/*
 AudioQueue 和 SDL 不一样
 AudioQueue要数据
 你自己定，最多 mAudioDataBytesCapacity
 上次消耗了多少mAudioDataByteSize
 */
// AudioQueue回调：buffer 播完回收后再填下一帧
void OutputBufferCallback(void *inUserData, AudioQueueRef inAQ, AudioQueueBufferRef inBuffer) {
    if (!inUserData || !inBuffer) {
        return;
    }
    
    printf("音频需要的 %d \n",inBuffer->mAudioDataByteSize);

    SCAudioQueuePlayer *queuePlayer = (__bridge SCAudioQueuePlayer *)inUserData;//C 对象 转OC对象
    SCPlayer *scp = queuePlayer->scp;
    
    if (!scp || scp->audioq.nb_packets <= 0) {
        /* 尚无数据：填静音并重新入队，避免队列饿死 */
        memset(inBuffer->mAudioData, 0, inBuffer->mAudioDataBytesCapacity);
        inBuffer->mAudioDataByteSize = inBuffer->mAudioDataBytesCapacity > 0
            ? (UInt32)FFMIN(BUFFER_SIZE, inBuffer->mAudioDataBytesCapacity)
            : 0;
        if (inBuffer->mAudioDataByteSize > 0 && queuePlayer->audioQueue) {
            AudioQueueEnqueueBuffer(queuePlayer->audioQueue, inBuffer, 0, NULL);
        }
        return;
    }
    
    //ffmpeg解码
    audio_decode_callback(scp,NULL,0);
    int buff_size = scp->out_audio_size;// 解码后的字节数
    if(!scp->audio_buf || buff_size <= 0) {
        if (buff_size <= 0) {
            AudioQueueStop(inAQ, false);
            NSLog(@"音频播放结束");
        }
        return;
    }
    
    //这个只是一个保护, 前提是scplayer每次多没有填满,所以每次添入的都被消耗了
    if (buff_size > (int)inBuffer->mAudioDataBytesCapacity) {
        buff_size = (int)inBuffer->mAudioDataBytesCapacity;
    }
    //ffmpeg中拷贝音频到iOS音频设备中
    inBuffer->mAudioDataByteSize = (UInt32)buff_size;
    memcpy(inBuffer->mAudioData, scp->audio_buf, (UInt32)buff_size);
    AudioQueueEnqueueBuffer(queuePlayer->audioQueue, inBuffer, 0, NULL);
    audio_queue_wrote(scp, buff_size);//增加硬件水位
}




- (void)initializeAudioQueue:(SCPlayer *)scp {
    self->scp = scp;

    /// 播放器播放时的ffmpeg采样格式
    /// 指定了播放器在读取数据时的数据长度(一帧多少个字节)
    AudioStreamBasicDescription asbd;
    /// 采样率
    asbd.mSampleRate = scp->audioInfo.freq;
    /// 音频流格式
    asbd.mFormatID = kAudioFormatLinearPCM;
    /// 每一帧音频格式的通道数
    asbd.mChannelsPerFrame = scp->audioInfo.channels;
    /// 一个pacet有多少个采样帧
    /// 一个采样帧就是一次声道数据采集
    /// PCM这个值是1
    asbd.mFramesPerPacket = 1;
    /// 每个通道一帧占的位宽
    asbd.mBitsPerChannel = 16;
    /// 每一帧所占的字节数
    asbd.mBytesPerFrame = 4;
    /// 一个packet所占的字节数
    asbd.mBytesPerPacket = asbd.mFramesPerPacket * asbd.mBytesPerFrame;
    /// kLinearPCMFormatFlagisSignedInteger: 存储的数据类型
    /// kAudioFormatFlagscpPacked: 数据交叉排列
    asbd.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    asbd.mReserved = 0;
    OSStatus status = AudioQueueNewOutput(&asbd,
                                          OutputBufferCallback,//iOS音频回掉函数设置
                                          (__bridge void *)(self),
                                          NULL,
                                          NULL,
                                          0, &audioQueue);
    
    NSAssert(status == noErr, @"Initialize audioQueue Failed");
    
    // 创建并分配音频缓冲区
    for (int i = 0; i < NUM_BUFFERS; i++) {
        status = AudioQueueAllocateBuffer(audioQueue, BUFFER_SIZE, &audioQueueBuffers[i]);
        if (status != noErr) {
            NSLog(@"分配AudioQueue缓冲区失败: %d", (int)status);
            AudioQueueDispose(audioQueue, true);
            audioQueue = NULL;
            return;
        }
        // 初始化填充音频数据到缓冲区
        OutputBufferCallback((__bridge void *)self, audioQueue, audioQueueBuffers[i]);
    }
    
    // 开始播放
    status = AudioQueueStart(audioQueue, NULL);
    if (status != noErr) {
        NSLog(@"启动AudioQueue失败: %d", (int)status);
        AudioQueueDispose(audioQueue, true);
        audioQueue = NULL;
        return;
    }
}


- (void)play {
    
}
- (void)stop {
    if (audioQueue) {
        AudioQueueStop(audioQueue, YES);
    }
}
- (void)pause {
    if (audioQueue) {
        AudioQueuePause(audioQueue);
    }
    NSLog(@"[音频]暂停");
}
- (void)resume {
    if (audioQueue) {
        AudioQueueStart(audioQueue, NULL);
    }
    NSLog(@"[音频]恢复");
}
- (void)cleanQueueCacheData {
    if (audioQueue) {
        AudioQueueFlush(audioQueue);
    }
}

- (void)dealloc {
    if (audioQueue) {
        AudioQueueStop(audioQueue, YES);
        AudioQueueDispose(audioQueue, true);
        audioQueue = NULL;
    }
}


@end
