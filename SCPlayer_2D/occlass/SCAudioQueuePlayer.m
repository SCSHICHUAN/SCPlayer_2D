//
//  SCAudioQueuePlayer.m
//  SCFFmpeg
//
//  Created by stan on 2024/7/11.
//  Copyright © 2024 石川. All rights reserved.
//

#import "SCAudioQueuePlayer.h"
#import <AudioToolbox/AudioToolbox.h>

#define NUM_BUFFERS 3
/*
 最小是音频帧的大小  =  一个音频帧采样个数 x （nb_channels）音频通道数 x 位深
      4096(byte)  =   1024 x 2 x 2(byte)
  预留更大缓冲，避免 resample 后一帧超过 4096 导致 memcpy 越界
 */
#define BUFFER_SIZE (4096 * 4)

static inline UInt32 sc_aq_min_u32(UInt32 a, UInt32 b) {
    return a < b ? a : b;
}

@implementation SCAudioQueuePlayer {
    AudioQueueRef audioQueue;
    AudioQueueBufferRef audioQueueBuffers[NUM_BUFFERS];
}

/*
 AudioQueue 和 SDL 不一样
 AudioQueue要数据
 你自己定，最多 mAudioDataBytesCapacity
 进回调时 mAudioDataByteSize ≈ 上一盒写入、刚播完的长度
 */
static void OutputBufferCallback(void *inUserData, AudioQueueRef inAQ, AudioQueueBufferRef inBuffer) {
    if (!inUserData || !inBuffer) {
        return;
    }

    SCAudioQueuePlayer *queuePlayer = (__bridge SCAudioQueuePlayer *)inUserData;
    Audion_queue_call_other audion_queue_call_other = queuePlayer.audion_queue_call_other;
    void *scp_player = queuePlayer.scp_player;

    if (!audion_queue_call_other || !scp_player || !queuePlayer->audioQueue) {
        return;
    }

    /* 上一盒已播完：通知业务扣水位（PCM 可能已覆盖，只传字节） */
    if (inBuffer->mAudioDataByteSize > 0) {
        audion_queue_call_other(scp_player, 1, (int)inBuffer->mAudioDataByteSize, NULL, NULL);
    }

    uint8_t *pcm = NULL;
    int pcmSize = 0;
    int ret = audion_queue_call_other(scp_player, 0, (int)inBuffer->mAudioDataBytesCapacity, &pcm, &pcmSize);

    if (ret == 1 || !pcm || pcmSize <= 0) {
        if (ret < 0) {
            AudioQueueStop(inAQ, false);
            NSLog(@"音频播放结束");
            return;
        }
        /* 暂无数据：填静音并重新入队，避免队列饿死（不计入业务水位） */
        memset(inBuffer->mAudioData, 0, inBuffer->mAudioDataBytesCapacity);
        inBuffer->mAudioDataByteSize = inBuffer->mAudioDataBytesCapacity > 0
            ? sc_aq_min_u32((UInt32)BUFFER_SIZE, inBuffer->mAudioDataBytesCapacity)
            : 0;
        if (inBuffer->mAudioDataByteSize > 0) {
            AudioQueueEnqueueBuffer(queuePlayer->audioQueue, inBuffer, 0, NULL);
        }
        return;
    }

    int buff_size = pcmSize;
    /* 保护：正常每帧 < capacity；截断仅防写越界 */
    if (buff_size > (int)inBuffer->mAudioDataBytesCapacity) {
        buff_size = (int)inBuffer->mAudioDataBytesCapacity;
    }
    // 拷贝数据到音频设备
    inBuffer->mAudioDataByteSize = (UInt32)buff_size;
    memcpy(inBuffer->mAudioData, pcm, (size_t)buff_size);
    AudioQueueEnqueueBuffer(queuePlayer->audioQueue, inBuffer, 0, NULL);
    /* 本盒已交给硬件 */
    audion_queue_call_other(scp_player, 2, buff_size, NULL, NULL);
}

- (void)initializeAudioQueueWithSampleRate:(double)sampleRate
                                  channels:(int)channels
                                  userData:(void *)userData {
    self.scp_player = userData;

    if (sampleRate <= 0 || channels <= 0) {
        NSLog(@"AudioQueue 参数非法: rate=%f channels=%d", sampleRate, channels);
        return;
    }

    if (audioQueue) {
        AudioQueueStop(audioQueue, YES);
        AudioQueueDispose(audioQueue, true);
        audioQueue = NULL;
    }

    AudioStreamBasicDescription asbd;
    asbd.mSampleRate = sampleRate;
    asbd.mFormatID = kAudioFormatLinearPCM;
    asbd.mChannelsPerFrame = (UInt32)channels;
    asbd.mFramesPerPacket = 1;
    asbd.mBitsPerChannel = 16;
    asbd.mBytesPerFrame = (UInt32)(channels * (asbd.mBitsPerChannel / 8));
    asbd.mBytesPerPacket = asbd.mFramesPerPacket * asbd.mBytesPerFrame;
    asbd.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    asbd.mReserved = 0;

    OSStatus status = AudioQueueNewOutput(&asbd,
                                          OutputBufferCallback,
                                          (__bridge void *)(self),
                                          NULL,
                                          NULL,
                                          0, &audioQueue);
    NSAssert(status == noErr, @"Initialize audioQueue Failed");

    for (int i = 0; i < NUM_BUFFERS; i++) {
        status = AudioQueueAllocateBuffer(audioQueue, BUFFER_SIZE, &audioQueueBuffers[i]);
        if (status != noErr) {
            NSLog(@"分配AudioQueue缓冲区失败: %d", (int)status);
            AudioQueueDispose(audioQueue, true);
            audioQueue = NULL;
            return;
        }
        OutputBufferCallback((__bridge void *)self, audioQueue, audioQueueBuffers[i]);
    }

    status = AudioQueueStart(audioQueue, NULL);
    if (status != noErr) {
        NSLog(@"启动AudioQueue失败: %d", (int)status);
        AudioQueueDispose(audioQueue, true);
        audioQueue = NULL;
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
