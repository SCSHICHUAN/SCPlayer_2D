//
//  ViewController.m
//  SCFFmpeg
//
//  Created by 石川 on 2019/5/18.
//  Copyright © 2019 石川. All rights reserved.
//

#import "ViewController.h"
#include "libavutil/log.h"
#include "libavformat/avio.h"
#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
#include "libswscale/swscale.h"
#include <AVKit/AVKit.h>
#import <OpenGLES/ES3/glext.h>
#import <GLKit/GLKit.h>
#import "SCRender.h"
#define kWidth ([UIScreen mainScreen].bounds.size.width)
#define kScal 1
#define kWH (1280/720.0)
//#define kWH (9/16.0)
#include "SCPlayer.h"
#import "SCAudioQueuePlayer.h"
#import "OCRender.h"

@interface ViewController ()
@property(nonatomic,assign)BOOL end;
@property(nonatomic,strong)UILabel *lab;
@property(nonatomic,assign)NSInteger video_pak_count;
@property(nonatomic,assign)NSInteger audio_pak_count;
@property(nonatomic,strong)NSTimer *timer;
@property(nonatomic,strong)SCRender *cRender;
@property(nonatomic,strong)OCRender *ocRender;
-(void)initAudio:(void *)opaque;
@end

@implementation ViewController


int when_frame_push(AVFrame *frame, int flag, void *opaque, void *userData){
    ViewController *vc = (__bridge ViewController *)userData;
    if(flag == 0){
        dispatch_async(dispatch_get_main_queue(), ^{
            [vc initAudio:opaque];
        });
    }else if(flag == 1){
        [vc.cRender displayWithFrame:frame bb:^(BOOL success) {
            if(success){
                VideoState *is = (VideoState*)opaque;
                fream_queue_pop(&is->pictq);
            }
        }];
    }
    
    //    printf("fram.size = %d,flat = %d \n",frame->pkt_size,flag);
    return 0;
}


-(void)initAudio:(void *)opaque{
    VideoState *is = (VideoState *)opaque;
    SCAudioQueuePlayer *aup = [[SCAudioQueuePlayer alloc] init];
    [aup initializeAudioQueue:is];
    [aup play];
}


-(void)testClick2{
    self.end = NO;
    self.video_pak_count = 0;
    self.audio_pak_count = 0;
    self.lab.text = @"拉流中请稍等...";
    [self.view addSubview:self.cRender];
    
    __weak typeof(self) weakSelf = self;
    dispatch_async(dispatch_get_global_queue(0, 0), ^{
        __strong typeof(weakSelf) strongSelf = weakSelf;
        if (!strongSelf) {
            return;
        }
        scplayer(when_frame_push, (__bridge void *)strongSelf);
    });
    
    
}
- (void)viewDidLoad {
    [super viewDidLoad];
    
    UIButton *test = [UIButton buttonWithType:UIButtonTypeCustom];
    [[UIApplication sharedApplication].keyWindow addSubview:test];
    test.frame = CGRectMake(50, 100, kWidth-100, 40);;
    test.backgroundColor = UIColor.blueColor;
    [test setTitle:@"START 开始拉流" forState:UIControlStateNormal];
    [test addTarget:self action:@selector(testClick2) forControlEvents:UIControlEventTouchUpInside];
    [self.view addSubview:test];
    
    UILabel *lab = [[UILabel alloc] initWithFrame: CGRectMake(50, 250, kWidth-100, 40)];
    lab.backgroundColor = UIColor.blackColor;
    lab.textColor = UIColor.whiteColor;
    [self.view addSubview:lab];
    self.lab = lab;
}



-(OCRender *)ocRender{
    if(!_ocRender){
        _ocRender = [[OCRender alloc] initWithFrame:self.view.bounds];
    }
    return _ocRender;
}

-(SCRender *)cRender{
    if(!_cRender){
        _cRender = [[SCRender alloc] initWithFrame:CGRectMake(0, 100, kWidth * kScal, kWidth * kWH * kScal)];
    }
    return _cRender;
}

@end
