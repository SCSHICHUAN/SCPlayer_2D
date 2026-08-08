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
   1 = 上一盒已播完（consumed），len = 上一盒字节
   2 = 本盒已 Enqueue（wrote），len = 本盒字节
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
