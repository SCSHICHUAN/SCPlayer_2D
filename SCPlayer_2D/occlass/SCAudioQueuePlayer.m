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
 */
#define BUFFER_SIZE 4096



SCAudioQueuePlayer *sc_self;

@implementation SCAudioQueuePlayer{
    AudioQueueRef audioQueue;
    struct FFAudioInformation audioInformation;
    CFMutableArrayRef buffers;
    AVStream *stream;
    
    
    AVFormatContext *formatContext;
    dispatch_queue_t decode_dispatch_queue;
    dispatch_queue_t audio_play_dispatch_queue;
    dispatch_queue_t video_render_dispatch_queue;
    AVPacket *packet;
    /// lock shared variate
    pthread_mutex_t mutex;
    double video_clock;
    double tolerance_scope;
    double audio_clock;
    
    AudioQueueBufferRef audioQueueBuffers[NUM_BUFFERS];
    FILE *pcmFile;
    VideoState *is;
}

// AudioQueue回调函数
void OutputBufferCallback(void *inUserData, AudioQueueRef inAQ, AudioQueueBufferRef inBuffer) {
    
    SCAudioQueuePlayer *scp = (__bridge SCAudioQueuePlayer *)inUserData;//C 对象 转OC对象
    VideoState *is = scp->is;
    if (is->audioq.nb_packets <= 0) {
        return;
    }
    
    sdl_audio_callback_1(is,NULL,0);
    int buff_size = is->out_audio_size;
    if(!is->audio_buf) return;
    
    
    inBuffer->mAudioDataByteSize = buff_size;
    memcpy(inBuffer->mAudioData, is->audio_buf, inBuffer->mAudioDataByteSize);
    AudioQueueEnqueueBuffer(scp->audioQueue, inBuffer, 0, NULL);
    
//    printf("buff_size:%d \n",buff_size);
    
    
    if(buff_size <=0) {
        // 如果文件读取结束，停止播放
        AudioQueueStop(inAQ, false);
        //        vc->audioQueueStarted = NO;
        NSLog(@"音频播放结束");
    }
    
    
    //    memcpy(inBuffer->mAudioData,is->audio_buf, 4096);
    //
    //    printf("bytesRead:%d \n",is->audio_buf_size);
    //    if (is->audio_buf_size > 0) {
    //        inBuffer->mAudioDataByteSize = 4096;
    //        AudioQueueEnqueueBuffer(inAQ, inBuffer, 0, NULL);
    //    } else {
    //        // 如果文件读取结束，停止播放
    //        AudioQueueStop(inAQ, false);
    ////        vc->audioQueueStarted = NO;
    //        NSLog(@"音频播放结束");
    //    }
}




- (void)initializeAudioQueue:(VideoState *)is {
    
    sc_self = self;
    self->is = is;
    
    
    
    /// 播放器播放时的ffmpeg采样格式
    /// 指定了播放器在读取数据时的数据长度(一帧多少个字节)
    AudioStreamBasicDescription asbd;
    /// 采样率
    asbd.mSampleRate = is->audioInfo.freq;
    /// 音频流格式
    asbd.mFormatID = kAudioFormatLinearPCM;
    /// 每一帧音频格式的通道数
    asbd.mChannelsPerFrame = is->audioInfo.channels;
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
    /// kLinearPCMFormatFlagIsSignedInteger: 存储的数据类型
    /// kAudioFormatFlagIsPacked: 数据交叉排列
    asbd.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    asbd.mReserved = 0;
    OSStatus status = AudioQueueNewOutput(&asbd,
                                          OutputBufferCallback,
                                          (__bridge void *)(self),
                                          NULL,
                                          NULL,
                                          0, &audioQueue);
    
    NSAssert(status == errSecSuccess, @"Initialize audioQueue Failed");
    
    // 创建并分配音频缓冲区
    for (int i = 0; i < NUM_BUFFERS; i++) {
        status = AudioQueueAllocateBuffer(audioQueue, BUFFER_SIZE, &audioQueueBuffers[i]);
        if (status != noErr) {
            NSLog(@"分配AudioQueue缓冲区失败: %d", (int)status);
            AudioQueueDispose(audioQueue, true);
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
        return;
    }
}


- (void)play {
    
}
- (void)stop {
    AudioQueueStop(audioQueue, YES);
}
- (void)pause {
    AudioQueuePause(audioQueue);
    NSLog(@"[音频]暂停");
}
- (void)resume {
    AudioQueueStart(audioQueue, NULL);
    NSLog(@"[音频]恢复");
}
- (void)cleanQueueCacheData {
    AudioQueueFlush(audioQueue);
}


@end
