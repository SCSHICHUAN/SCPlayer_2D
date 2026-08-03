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
#include "SCPlayer.h"
#import "OCRender.h"

ViewController *c_self;


int isOC = 0;

@interface ViewController ()
@property(nonatomic,assign)BOOL end;
@property(nonatomic,strong)UILabel *lab;
@property(nonatomic,assign)NSInteger video_pak_count;
@property(nonatomic,assign)NSInteger audio_pak_count;
@property(nonatomic,strong)NSTimer *timer;
@property(nonatomic,strong)SCRender *cRender;
@property(nonatomic,strong)OCRender *ocRender;
@end

@implementation ViewController
-(NSTimer *)timer{
    if(!_timer){
        _timer = [NSTimer scheduledTimerWithTimeInterval:1/60 target:self selector:@selector(update) userInfo:nil repeats:YES];
        [[NSRunLoop currentRunLoop] addTimer:_timer forMode:NSRunLoopCommonModes];
    }
    return _timer;
}


-(void)open{
    dispatch_async(dispatch_get_main_queue(), ^{
        self.lab.text = @"写入音频平数据中....";
        NSString *document = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
        NSLog(@"document=%@",document);
    });
}
-(void)open2{
    dispatch_async(dispatch_get_main_queue(), ^{
        AVPlayerViewController *pvc = [[AVPlayerViewController alloc] init];
        NSString *document = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
        NSString *path = [document stringByAppendingPathComponent:@"sc.mp4"];
        NSURL *url = [[NSURL alloc] initFileURLWithPath:path];
        pvc.player = [[AVPlayer alloc] initWithURL:url];
        [pvc.player play];
        [self presentViewController:pvc animated:YES completion:nil];
    });
}


int when_frame_push(AVFrame *frame, int flag,void *opaque){
    if(flag == 0){
         dispatch_async(dispatch_get_main_queue(), ^{
            [c_self initAudio:opaque];
        });
    }else if(flag == 1){
        
        if(isOC){
            [c_self.ocRender displayWithFrame:frame bb:^(BOOL success) {
                if(success){
                    VideoState *is = (VideoState*)opaque;
                    fream_queue_pop(&is->pictq);
                }
            }];
        }else{
            [c_self.cRender displayWithFrame:frame bb:^(BOOL success) {
                if(success){
                    VideoState *is = (VideoState*)opaque;
                    fream_queue_pop(&is->pictq);
                }
            }];
        }
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




int i = 1;

-(void)testClick2{
    self.end = NO;
    self.video_pak_count = 0;
    self.audio_pak_count = 0;
    self.lab.text = @"拉流中请稍等...";
    if(isOC){
        [self.view addSubview:self.ocRender];
    }else{
        [self.view addSubview:self.cRender];
    }
    

    
    dispatch_async(dispatch_get_global_queue(0, 0), ^{
        scplayer(when_frame_push);
    });
   
    
}
- (void)viewDidLoad {
    [super viewDidLoad];
    
    {
        UIButton *test = [UIButton buttonWithType:UIButtonTypeCustom];
        [[UIApplication sharedApplication].keyWindow addSubview:test];
        test.frame = CGRectMake(50, 100, kWidth-100, 40);;
        test.backgroundColor = UIColor.blueColor;
        [test setTitle:@"START 开始拉流" forState:UIControlStateNormal];
        [test addTarget:self action:@selector(testClick2) forControlEvents:UIControlEventTouchUpInside];
        [self.view addSubview:test];
    }
    UIButton *test = [UIButton buttonWithType:UIButtonTypeCustom];
    [[UIApplication sharedApplication].keyWindow addSubview:test];
    test.frame = CGRectMake(50, 150, 100, 40);
    test.backgroundColor = UIColor.redColor;
    [test setTitle:@"前进" forState:UIControlStateNormal];
    [test addTarget:self action:@selector(testClick) forControlEvents:UIControlEventTouchDown];
    [test addTarget:self action:@selector(testClick) forControlEvents:UIControlEventTouchUpInside];
    [self.view addSubview:test];
    {
        UIButton *test = [UIButton buttonWithType:UIButtonTypeCustom];
        [[UIApplication sharedApplication].keyWindow addSubview:test];
        test.frame = CGRectMake(50+120, 150, 100, 40);
        test.backgroundColor = UIColor.redColor;
        [test setTitle:@"后退" forState:UIControlStateNormal];
        [test addTarget:self action:@selector(testClick1) forControlEvents:UIControlEventTouchDown];
        [test addTarget:self action:@selector(testClick1) forControlEvents:UIControlEventTouchUpInside];
        [self.view addSubview:test];
    }
    {
        UIButton *test = [UIButton buttonWithType:UIButtonTypeCustom];
        [[UIApplication sharedApplication].keyWindow addSubview:test];
        test.frame = CGRectMake(50, 200, 100, 40);
        test.backgroundColor = UIColor.redColor;
        [test setTitle:@"左" forState:UIControlStateNormal];
        [test addTarget:self action:@selector(testClick3) forControlEvents:UIControlEventTouchDown];
        [test addTarget:self action:@selector(testClick3) forControlEvents:UIControlEventTouchUpInside];
        [self.view addSubview:test];
    }
    {
        UIButton *test = [UIButton buttonWithType:UIButtonTypeCustom];
        [[UIApplication sharedApplication].keyWindow addSubview:test];
        test.frame = CGRectMake(50+120, 200, 100, 40);
        test.backgroundColor = UIColor.redColor;
        [test setTitle:@"右" forState:UIControlStateNormal];
        [test addTarget:self action:@selector(testClick4) forControlEvents:UIControlEventTouchDown];
        [test addTarget:self action:@selector(testClick4) forControlEvents:UIControlEventTouchUpInside];
        [self.view addSubview:test];
    }
    
    {
        UIButton *test = [UIButton buttonWithType:UIButtonTypeCustom];
        [[UIApplication sharedApplication].keyWindow addSubview:test];
        test.frame = CGRectMake(50+120+105, 200, 100, 40);
        test.backgroundColor = UIColor.redColor;
        [test setTitle:@"自转" forState:UIControlStateNormal];
        [test addTarget:self action:@selector(testClick5) forControlEvents:UIControlEventTouchDown];
        [test addTarget:self action:@selector(testClick5) forControlEvents:UIControlEventTouchUpInside];
        [self.view addSubview:test];
    }
    
    
    UILabel *lab = [[UILabel alloc] initWithFrame: CGRectMake(50, 250, kWidth-100, 40)];
    lab.backgroundColor = UIColor.blackColor;
    lab.textColor = UIColor.whiteColor;
    [self.view addSubview:lab];
    self.lab = lab;
    c_self = self;
}



-(void)testClick{
    self.cRender.forward_B = !self.cRender.forward_B;
    NSLog(@"前进");
}
-(void)testClick1{
    self.cRender.back_B = !self.cRender.back_B;
}

-(void)testClick3{
    self.cRender.left_B = !self.cRender.left_B;
}
-(void)testClick4{
    self.cRender.right_B = !self.cRender.right_B;
}
-(void)testClick5{
    self.cRender.right_R_B = !self.cRender.right_R_B;
}

//-(void)update{
//    if(forward_B){
//        render.forward+=1;
//    }
//    if(back_B){
//        render.back+=1;
//    }
//    if(left_B){
//        render.left+=1;
//    }
//    if(right_B){
//        render.right+=1;
//    }
//    if(right_R_B){
//        render.right_R+=1;
//    }
//
//}

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
