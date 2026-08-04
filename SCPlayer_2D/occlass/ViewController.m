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
#import <AVFoundation/AVFoundation.h>
#import <OpenGLES/ES3/glext.h>
#import <GLKit/GLKit.h>
#import <MobileCoreServices/MobileCoreServices.h>
#import "SCRender.h"
#define kWidth ([UIScreen mainScreen].bounds.size.width)
#define kScal 1
#define kWH (1280/720.0)
//#define kWH (9/16.0)
#include "SCPlayer.h"
#import "SCAudioQueuePlayer.h"


@interface ViewController () <UIImagePickerControllerDelegate, UINavigationControllerDelegate>
@property(nonatomic,assign)BOOL end;
@property(nonatomic,strong)UILabel *lab;
@property(nonatomic,assign)NSInteger video_pak_count;
@property(nonatomic,assign)NSInteger audio_pak_count;
@property(nonatomic,strong)NSTimer *timer;
@property(nonatomic,strong)SCRender *cRender;
@property(nonatomic,strong)SCAudioQueuePlayer *audioPlayer; /* AudioQueue 回调持有 self，必须强引用 */
@property(nonatomic,copy)NSString *playingPath;
-(void)initAudio:(void *)opaque;
-(void)startPlayWithPath:(NSString *)path;
@end

@implementation ViewController


int when_frame_push(AVFrame *frame, int flag, void *opaque, void *userData){
    ViewController *vc = (__bridge ViewController *)userData;
    if(flag == 0){
        dispatch_async(dispatch_get_main_queue(), ^{
            [vc initAudio:opaque];
        });
    }else if(flag == 1){
        /* 模块已占 display_busy；接入方只负责：释放拷贝 + 清 busy */
        VideoState *is = (VideoState *)opaque;
        if (!frame) {
            if (is) {
                is->display_busy = 0;
            }
            return 0;
        }
        [vc.cRender displayWithFrame:frame bb:^(BOOL success) {
            (void)success;
            AVFrame *owned = frame;
            av_frame_free(&owned);
            if (is) {
                is->display_busy = 0;
            }
        }];
    }
    
    //    printf("fram.size = %d,flat = %d \n",frame->pkt_size,flag);
    return 0;
}


-(void)initAudio:(void *)opaque{
    VideoState *is = (VideoState *)opaque;
    if (!is) {
        return;
    }
    /* 不可用局部变量：initialize 后 AudioQueue 仍回调 self，局部对象会被 ARC 释放导致野指针崩溃 */
    [self.audioPlayer stop];
    self.audioPlayer = [[SCAudioQueuePlayer alloc] init];
    [self.audioPlayer initializeAudioQueue:is];
    [self.audioPlayer play];
}

-(void)startPlayWithPath:(NSString *)path{
    if (path.length == 0) {
        self.lab.text = @"无效的视频路径";
        return;
    }
    if (![[NSFileManager defaultManager] fileExistsAtPath:path]) {
        self.lab.text = @"文件不存在";
        return;
    }

    self.end = NO;
    self.video_pak_count = 0;
    self.audio_pak_count = 0;
    self.playingPath = path;
    self.lab.text = [NSString stringWithFormat:@"播放中: %@", path.lastPathComponent];
    [self.view addSubview:self.cRender];

    __weak typeof(self) weakSelf = self;
    dispatch_async(dispatch_get_global_queue(0, 0), ^{
        __strong typeof(weakSelf) strongSelf = weakSelf;
        if (!strongSelf) {
            return;
        }
        scplayer([path UTF8String], when_frame_push, (__bridge void *)strongSelf);
    });
}

-(void)testClick2{
//    [self startPlayWithPath:@"/Users/stan/Desktop/2016物理院同学会/追光者不如见一面.mp4"];
    [self startPlayWithPath:@"/Users/stan/Desktop/ffop.mp4"];
}

-(void)pickAlbumVideo{
    if (![UIImagePickerController isSourceTypeAvailable:UIImagePickerControllerSourceTypePhotoLibrary]) {
        self.lab.text = @"当前设备不支持相册";
        return;
    }

    UIImagePickerController *picker = [[UIImagePickerController alloc] init];
    picker.sourceType = UIImagePickerControllerSourceTypePhotoLibrary;
    picker.mediaTypes = @[(NSString *)kUTTypeMovie];
    picker.delegate = self;
    picker.allowsEditing = NO;
    /* Passthrough：不重编码，导出/拷贝很快 */
    if (@available(iOS 11.0, *)) {
        picker.videoExportPreset = AVAssetExportPresetPassthrough;
    }
    picker.modalPresentationStyle = UIModalPresentationFullScreen;
    [self presentViewController:picker animated:YES completion:nil];
}

#pragma mark - UIImagePickerControllerDelegate

- (void)imagePickerController:(UIImagePickerController *)picker
didFinishPickingMediaWithInfo:(NSDictionary<UIImagePickerControllerInfoKey,id> *)info {
    NSURL *mediaURL = info[UIImagePickerControllerMediaURL];

    NSString *localPath = nil;
    if (mediaURL) {
        /* 回调里立刻拷进 App tmp（此时可读），再 dismiss 播放 */
        NSString *ext = mediaURL.pathExtension.length ? mediaURL.pathExtension : @"MOV";
        NSString *name = [NSString stringWithFormat:@"album_%@.%@",
                          @((long long)([[NSDate date] timeIntervalSince1970] * 1000)), ext];
        localPath = [NSTemporaryDirectory() stringByAppendingPathComponent:name];
        [[NSFileManager defaultManager] removeItemAtPath:localPath error:nil];
        NSError *err = nil;
        if (![[NSFileManager defaultManager] copyItemAtURL:mediaURL
                                                     toURL:[NSURL fileURLWithPath:localPath]
                                                     error:&err]) {
            localPath = nil;
            self.lab.text = err.localizedDescription ?: @"拷贝相册视频失败";
        }
    } else {
        self.lab.text = @"未获取到视频文件";
    }

    NSString *pathToPlay = localPath;
    [picker dismissViewControllerAnimated:YES completion:^{
        if (pathToPlay.length) {
            [self startPlayWithPath:pathToPlay];
        }
    }];
}

- (void)imagePickerControllerDidCancel:(UIImagePickerController *)picker {
    [picker dismissViewControllerAnimated:YES completion:nil];
    self.lab.text = @"已取消选择";
}

- (void)viewDidLoad {
    [super viewDidLoad];

    UIButton *test = [UIButton buttonWithType:UIButtonTypeCustom];
    test.frame = CGRectMake(50, 50, kWidth - 100, 40);
    test.backgroundColor = UIColor.blueColor;
    [test setTitle:@"START 本地示例" forState:UIControlStateNormal];
    [test addTarget:self action:@selector(testClick2) forControlEvents:UIControlEventTouchUpInside];
    [self.view addSubview:test];

    UIButton *albumBtn = [UIButton buttonWithType:UIButtonTypeCustom];
    albumBtn.frame = CGRectMake(50, 100, kWidth - 100, 40);
    albumBtn.backgroundColor = [UIColor colorWithRed:0.16 green:0.65 blue:0.45 alpha:1.0];
    [albumBtn setTitle:@"选择相册视频" forState:UIControlStateNormal];
    [albumBtn addTarget:self action:@selector(pickAlbumVideo) forControlEvents:UIControlEventTouchUpInside];
    [self.view addSubview:albumBtn];

    UILabel *lab = [[UILabel alloc] initWithFrame:CGRectMake(50, 150, kWidth - 100, 40)];
    lab.backgroundColor = UIColor.blackColor;
    lab.textColor = UIColor.whiteColor;
    lab.font = [UIFont systemFontOfSize:13];
    lab.adjustsFontSizeToFitWidth = YES;
    [self.view addSubview:lab];
    self.lab = lab;
}

-(SCRender *)cRender{
    if(!_cRender){
        /* 放在按钮下方，避免挡住操作 */
        _cRender = [[SCRender alloc] initWithFrame:CGRectMake(0, 200, kWidth * kScal, kWidth * kWH * kScal)];
    }
    return _cRender;
}

@end
