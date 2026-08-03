//
//  SCAudioQueuePlayer.h
//  SCFFmpeg
//
//  Created by stan on 2024/7/11.
//  Copyright © 2024 石川. All rights reserved.
//

#import <Foundation/Foundation.h>
#include "SCPlayer.h"

NS_ASSUME_NONNULL_BEGIN

@interface SCAudioQueuePlayer : NSObject

- (void)initializeAudioQueue:(VideoState *)is;
- (void)play;
- (void)stop;
- (void)pause ;
- (void)resume;
- (void)cleanQueueCacheData;

@end

NS_ASSUME_NONNULL_END
