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
 解耦
 flag:
   0 = 解码取 PCM：写入 *outPCM / *outSize；返回 0 有数据，1 暂无（填静音），-1 结束停播
   1 = 保留（当前设备层不再调用；水位不走此路径）
   2 = 本盒已 Enqueue（wrote）：入队记账 + 对齐上一盒 + 开本盒 0~len 插值
 outPCM / outSize 仅 flag==0 时使用，其余可传 NULL
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
