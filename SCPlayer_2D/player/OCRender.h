//
//  OCRender.h
//  SCFFmpeg
//
//  Created by stan on 2024/8/2.
//  Copyright © 2024 石川. All rights reserved.
//

#import <UIKit/UIKit.h>
#include <libavformat/avformat.h>
NS_ASSUME_NONNULL_BEGIN

@interface OCRender : UIView


- (void)displayWithFrame:(AVFrame *)yuvFrame bb:(void (^)(BOOL success))completionBlock;


@end

NS_ASSUME_NONNULL_END
