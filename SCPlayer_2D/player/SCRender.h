//
//  SCRender.h
//  SCFFmpeg
//
//  Created by stan on 2024/1/17.
//  Copyright © 2024 石川. All rights reserved.
//

#import <UIKit/UIKit.h>
#include "libavformat/avformat.h"
NS_ASSUME_NONNULL_BEGIN

typedef NS_ENUM(NSInteger, SCRenderFillMode) {
    SCRenderFillModeAspectFit = 0,   /* 等比例完整显示，留黑边 */
    SCRenderFillModeScaleToFill = 1, /* 拉伸铺满，可能变形 */
};

/* 画质：只控制 contentsScale（分辨率；超清=超采样高于屏） */
typedef NS_ENUM(NSInteger, SCRenderQuality) {
    SCRenderQualityFluent = 0,   /* scale=1 */
    SCRenderQualityBalanced = 1, /* scale≈min(2,原生) */
    SCRenderQualityHigh = 2,     /* scale=原生屏 */
    SCRenderQualityUltra = 3,    /* scale=min(原生×1.5, 4)，更锐但更吃 GPU */
};

/* 抗锯齿档位：独立于画质；8x 视设备 GL_MAX_SAMPLES */
typedef NS_ENUM(NSInteger, SCRenderMSAALevel) {
    SCRenderMSAAOff = 0,  /* 关闭 */
    SCRenderMSAA2x = 1,   /* 2x */
    SCRenderMSAA4x = 2,   /* 4x */
    SCRenderMSAA8x = 3,   /* 8x（不支持则自动降到设备上限） */
};

@interface SCRender : UIView


- (void)displayWithFrame:(AVFrame *)yuvFrame bb:(void (^)(BOOL success))completionBlock;
- (void)clearDisplay; /* 清黑屏，切换片源时用 */
@property(nonatomic,assign) SCRenderFillMode fillMode;
@property(nonatomic,assign) SCRenderQuality quality;
@property(nonatomic,assign) SCRenderMSAALevel msaaLevel;
@property(nonatomic,assign) int rotateDegrees; /* 0/90/180/270，流元数据旋转纠正 */
@property(nonatomic,assign) double lastrenderMS; /* 最近一帧 GL 总耗时（ms） */
@property(nonatomic,assign)float testD;
@property(nonatomic,assign)float forward;
@property(nonatomic,assign)float back;
@property(nonatomic,assign)float right;
@property(nonatomic,assign)float left;
@property(nonatomic,assign)float right_R;


@property(nonatomic,assign)bool forward_B;
@property(nonatomic,assign)bool back_B;
@property(nonatomic,assign)bool left_B;
@property(nonatomic,assign)bool right_B;
@property(nonatomic,assign)bool right_R_B;

@end

NS_ASSUME_NONNULL_END
