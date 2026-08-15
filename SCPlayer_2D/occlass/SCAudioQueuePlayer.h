//
//  SCAudioQueuePlayer.h
//  SCFFmpeg
//
//  Created by stan on 2024/7/11.
//  Copyright © 2024 石川. All rights reserved.
//

#import <Foundation/Foundation.h>
#import <stdint.h>

NS_ASSUME_NONNULL_BEGIN

/*
 设备层(SCAudioQueuePlayer) ↔ 播放器(SCPlayer) 解耦：
 - 设备只负责：问要数据、memcpy 进 AQ buffer、Enqueue；不碰解码/补偿/时钟
 - 播放器只负责：产出 PCM（解码/补帧）和 wrote 记账；不碰 AudioQueue API
 - 返回值 = 状态(0有数/1暂无/‑1结束)；PCM 用输出参数 *outPCM / *outSize 带出
   （C 单返回值不够同时传状态+指针+长度，故用二级指针写回调用方的 pcm）
 flag:
   0 = 解码取 PCM：写入 *outPCM / *outSize
   1 = 保留（当前设备层不再调用）
   2 = 本盒已 Enqueue（wrote）：入队记账；outPCM/outSize 传 NULL
 outPCM / outSize 仅 flag==0 时使用
 */
typedef int (*Audion_queue_call_other)(void *userData,
                                       int flag,
                                       int len,
                                       uint8_t *_Nullable *_Nullable outPCM,
                                       int *_Nullable outSize);

@interface SCAudioQueuePlayer : NSObject

@property (nonatomic, assign, nullable) Audion_queue_call_other audion_queue_call_other;
@property (nonatomic, assign, nullable) void *scp_player;

/// 冷启动预热 AVAudioSession（Playback + setActive）。首播才激活硬件易爆音，切片后已热则正常。
+ (void)warmUpAudioSession;

/// 仅传设备参数 + 上下文，不依赖 SCPlayer 类型
- (void)initializeAudioQueueWithSampleRate:(double)sampleRate
                                  channels:(int)channels
                                  userData:(nullable void *)userData;
- (void)play;
- (void)stop;
- (void)pause;
- (void)resume;
- (void)cleanQueueCacheData;

@end

NS_ASSUME_NONNULL_END
