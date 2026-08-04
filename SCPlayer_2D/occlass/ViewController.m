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
#include "SCPlayer.h"
#import "SCAudioQueuePlayer.h"
#import "SCDropdownButton.h"


@interface ViewController () <UIImagePickerControllerDelegate, UINavigationControllerDelegate, UITextFieldDelegate>
@property(nonatomic,assign)BOOL end;
@property(nonatomic,strong)UILabel *lab;
@property(nonatomic,assign)NSInteger video_pak_count;
@property(nonatomic,assign)NSInteger audio_pak_count;
@property(nonatomic,strong)NSTimer *timer;
@property(nonatomic,strong)SCRender *cRender;
@property(nonatomic,strong)SCAudioQueuePlayer *audioPlayer; /* AudioQueue 回调持有 self，必须强引用 */
@property(nonatomic,copy)NSString *playingPath;
@property(nonatomic,strong)UIButton *modeBtn;
@property(nonatomic,strong)SCDropdownButton *qualityDropdown;
@property(nonatomic,strong)SCDropdownButton *msaaDropdown;
@property(nonatomic,strong)UITextField *urlField;
@property(nonatomic,strong)UIView *controlsBar;
@property(nonatomic,assign)BOOL controlsVisible;
@property(nonatomic,assign)VideoState *playingIs;
-(void)initAudio:(void *)opaque;
-(void)startPlayWithPath:(NSString *)path;
-(void)playURLFromField;
-(BOOL)isNetworkURL:(NSString *)s;
-(void)stopCurrentPlayback;
-(void)toggleRenderMode;
-(void)updateModeButtonTitle;
-(UIButton *)makeButton:(NSString *)title action:(SEL)action;
-(void)buildControls;
-(void)toggleControlsVisibility;
-(void)onCenterTap:(UITapGestureRecognizer *)gr;
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
        if (!is || is->quit) {
            av_frame_free(&frame);
            if (is) {
                is->display_busy = 0;
            }
            return 0;
        }
        vc.playingIs = is;
        vc.cRender.rotateDegrees = is->video_rotate;
        [vc.cRender displayWithFrame:frame bb:^(BOOL success) {
            (void)success;
            AVFrame *owned = frame;
            av_frame_free(&owned);
            /* 已切走片源则不要再写已释放的 VideoState */
            if (vc.playingIs == is) {
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

-(void)stopCurrentPlayback{
    VideoState *is = self.playingIs;
    self.playingIs = NULL;
    [self.audioPlayer stop];
    self.audioPlayer = nil;
    self.cRender.rotateDegrees = 0;
    [self.cRender clearDisplay];
    if (is) {
        is->display_busy = 0;
        scplayer_stop(is);
    }
}

-(BOOL)isNetworkURL:(NSString *)s{
    NSString *lower = s.lowercaseString;
    return [lower hasPrefix:@"http://"]
        || [lower hasPrefix:@"https://"]
        || [lower hasPrefix:@"rtmp://"]
        || [lower hasPrefix:@"rtsp://"]
        || [lower hasPrefix:@"udp://"]
        || [lower hasPrefix:@"tcp://"];
}

-(void)startPlayWithPath:(NSString *)path{
    path = [path stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
    if (path.length == 0) {
        self.lab.text = @"无效的视频路径";
        return;
    }
    BOOL network = [self isNetworkURL:path];
    if (!network && ![[NSFileManager defaultManager] fileExistsAtPath:path]) {
        self.lab.text = @"文件不存在";
        return;
    }

    /* 切换片源：先停旧实例并清屏 */
    [self stopCurrentPlayback];
    [self.view endEditing:YES];

    self.end = NO;
    self.video_pak_count = 0;
    self.audio_pak_count = 0;
    self.playingPath = path;
    if (network) {
        self.lab.text = [NSString stringWithFormat:@"播放 URL: %@", path];
    } else {
        self.lab.text = [NSString stringWithFormat:@"播放中: %@", path.lastPathComponent];
    }

    if (!self.cRender.superview) {
        [self.view insertSubview:self.cRender atIndex:0];
    } else {
        [self.view sendSubviewToBack:self.cRender];
    }
    [self.view bringSubviewToFront:self.controlsBar];

    __weak typeof(self) weakSelf = self;
    dispatch_async(dispatch_get_global_queue(0, 0), ^{
        __strong typeof(weakSelf) strongSelf = weakSelf;
        if (!strongSelf) {
            return;
        }
        int ret = scplayer([path UTF8String], when_frame_push, (__bridge void *)strongSelf);
        if (ret != 0) {
            dispatch_async(dispatch_get_main_queue(), ^{
                strongSelf.lab.text = @"打开失败，请检查路径或 URL";
            });
        }
    });
}

-(void)playURLFromField{
    [self startPlayWithPath:self.urlField.text];
}

-(void)testClick2{
//    [self startPlayWithPath:@"/Users/stan/Desktop/2016物理院同学会/追光者不如见一面.mp4"];
    [self startPlayWithPath:@"/Users/stan/Desktop/ffop.mp4"];
}

-(void)toggleRenderMode{
    if (self.cRender.fillMode == SCRenderFillModeAspectFit) {
        self.cRender.fillMode = SCRenderFillModeScaleToFill;
    } else {
        self.cRender.fillMode = SCRenderFillModeAspectFit;
    }
    [self updateModeButtonTitle];
}

-(void)updateModeButtonTitle{
    NSString *title = (self.cRender.fillMode == SCRenderFillModeAspectFit)
        ? @"等比例"
        : @"拉伸铺满";
    [self.modeBtn setTitle:title forState:UIControlStateNormal];
}

/* 与 GL-ARKit makeButton 同风格：半透明黑底 + 白字 + 圆角 */
-(UIButton *)makeButton:(NSString *)title action:(SEL)action {
    UIButton *b = [UIButton buttonWithType:UIButtonTypeSystem];
    [b setTitle:title forState:UIControlStateNormal];
    b.titleLabel.font = [UIFont monospacedDigitSystemFontOfSize:14 weight:UIFontWeightSemibold];
    b.backgroundColor = [[UIColor blackColor] colorWithAlphaComponent:0.45];
    [b setTitleColor:UIColor.whiteColor forState:UIControlStateNormal];
    b.layer.cornerRadius = 8;
    b.contentEdgeInsets = UIEdgeInsetsMake(8, 10, 8, 10);
    if (action) {
        [b addTarget:self action:action forControlEvents:UIControlEventTouchUpInside];
    }
    return b;
}

-(void)buildControls{
    UIView *bar = [[UIView alloc] initWithFrame:CGRectZero];
    bar.translatesAutoresizingMaskIntoConstraints = NO;
    bar.backgroundColor = [[UIColor blackColor] colorWithAlphaComponent:0.25];
    bar.layer.cornerRadius = 10;
    [self.view addSubview:bar];
    self.controlsBar = bar;

    UIButton *startBtn = [self makeButton:@"本地示例" action:@selector(testClick2)];
    UIButton *albumBtn = [self makeButton:@"相册视频" action:@selector(pickAlbumVideo)];
    UIButton *modeBtn = [self makeButton:@"等比例" action:@selector(toggleRenderMode)];
    self.modeBtn = modeBtn;

    SCDropdownButton *qualityDrop =
        [[SCDropdownButton alloc] initWithPrefix:@"画质"
                                         options:@[@"流畅", @"均衡", @"高清", @"超清"]
                                   selectedIndex:SCRenderQualityBalanced];
    qualityDrop.panelAlignment = SCDropdownPanelAlignmentLeading;
    __weak typeof(self) weakSelf = self;
    qualityDrop.selectionHandler = ^(NSInteger index, NSString *title) {
        (void)title;
        __strong typeof(weakSelf) strongSelf = weakSelf;
        if (!strongSelf) return;
        strongSelf.cRender.quality = (SCRenderQuality)index;
        strongSelf.lab.text = [NSString stringWithFormat:@"画质: %@", title];
    };
    self.qualityDropdown = qualityDrop;

    SCDropdownButton *msaaDrop =
        [[SCDropdownButton alloc] initWithPrefix:@"抗锯齿"
                                         options:@[@"关", @"2x", @"4x", @"8x"]
                                   selectedIndex:SCRenderMSAA4x];
    msaaDrop.panelAlignment = SCDropdownPanelAlignmentLeading;
    msaaDrop.selectionHandler = ^(NSInteger index, NSString *title) {
        (void)title;
        __strong typeof(weakSelf) strongSelf = weakSelf;
        if (!strongSelf) return;
        strongSelf.cRender.msaaLevel = (SCRenderMSAALevel)index;
        strongSelf.lab.text = [NSString stringWithFormat:@"抗锯齿: %@", title];
    };
    self.msaaDropdown = msaaDrop;

    UILabel *lab = [[UILabel alloc] initWithFrame:CGRectZero];
    lab.translatesAutoresizingMaskIntoConstraints = NO;
    lab.textColor = [UIColor colorWithWhite:1 alpha:0.9];
    lab.font = [UIFont monospacedDigitSystemFontOfSize:12 weight:UIFontWeightRegular];
    lab.numberOfLines = 2;
    lab.adjustsFontSizeToFitWidth = YES;
    lab.text = @"点屏幕中心可显隐控件";
    self.lab = lab;

    startBtn.translatesAutoresizingMaskIntoConstraints = NO;
    albumBtn.translatesAutoresizingMaskIntoConstraints = NO;
    modeBtn.translatesAutoresizingMaskIntoConstraints = NO;
    [bar addSubview:startBtn];
    [bar addSubview:albumBtn];
    [bar addSubview:modeBtn];
    [bar addSubview:qualityDrop];
    [bar addSubview:msaaDrop];
    [bar addSubview:lab];

    UITextField *urlField = [[UITextField alloc] initWithFrame:CGRectZero];
    urlField.translatesAutoresizingMaskIntoConstraints = NO;
    urlField.backgroundColor = [[UIColor blackColor] colorWithAlphaComponent:0.45];
    urlField.textColor = UIColor.whiteColor;
    urlField.tintColor = UIColor.whiteColor;
    urlField.font = [UIFont monospacedDigitSystemFontOfSize:13 weight:UIFontWeightRegular];
    urlField.attributedPlaceholder = [[NSAttributedString alloc]
                                      initWithString:@"输入 http(s)/rtmp/rtsp URL"
                                      attributes:@{NSForegroundColorAttributeName: [UIColor colorWithWhite:1 alpha:0.45]}];
    urlField.keyboardType = UIKeyboardTypeURL;
    urlField.autocapitalizationType = UITextAutocapitalizationTypeNone;
    urlField.autocorrectionType = UITextAutocorrectionTypeNo;
    urlField.clearButtonMode = UITextFieldViewModeWhileEditing;
    urlField.returnKeyType = UIReturnKeyGo;
    urlField.layer.cornerRadius = 8;
    urlField.leftView = [[UIView alloc] initWithFrame:CGRectMake(0, 0, 10, 1)];
    urlField.leftViewMode = UITextFieldViewModeAlways;
    urlField.delegate = self;
    self.urlField = urlField;

    UIButton *urlPlayBtn = [self makeButton:@"播放URL" action:@selector(playURLFromField)];
    urlPlayBtn.translatesAutoresizingMaskIntoConstraints = NO;
    [bar addSubview:urlField];
    [bar addSubview:urlPlayBtn];

    UILayoutGuide *safe = self.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [bar.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:12],
        [bar.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-12],
        [bar.topAnchor constraintEqualToAnchor:safe.topAnchor constant:8],

        [startBtn.leadingAnchor constraintEqualToAnchor:bar.leadingAnchor constant:10],
        [startBtn.topAnchor constraintEqualToAnchor:bar.topAnchor constant:10],
        [startBtn.heightAnchor constraintEqualToConstant:36],

        [albumBtn.leadingAnchor constraintEqualToAnchor:startBtn.trailingAnchor constant:8],
        [albumBtn.centerYAnchor constraintEqualToAnchor:startBtn.centerYAnchor],
        [albumBtn.heightAnchor constraintEqualToConstant:36],

        [modeBtn.leadingAnchor constraintEqualToAnchor:albumBtn.trailingAnchor constant:8],
        [modeBtn.centerYAnchor constraintEqualToAnchor:startBtn.centerYAnchor],
        [modeBtn.heightAnchor constraintEqualToConstant:36],

        [qualityDrop.leadingAnchor constraintEqualToAnchor:modeBtn.trailingAnchor constant:8],
        [qualityDrop.centerYAnchor constraintEqualToAnchor:startBtn.centerYAnchor],

        [msaaDrop.leadingAnchor constraintEqualToAnchor:bar.leadingAnchor constant:10],
        [msaaDrop.topAnchor constraintEqualToAnchor:startBtn.bottomAnchor constant:8],

        [lab.leadingAnchor constraintEqualToAnchor:msaaDrop.trailingAnchor constant:8],
        [lab.trailingAnchor constraintEqualToAnchor:bar.trailingAnchor constant:-10],
        [lab.centerYAnchor constraintEqualToAnchor:msaaDrop.centerYAnchor],

        [urlField.leadingAnchor constraintEqualToAnchor:bar.leadingAnchor constant:10],
        [urlField.topAnchor constraintEqualToAnchor:msaaDrop.bottomAnchor constant:8],
        [urlField.heightAnchor constraintEqualToConstant:36],

        [urlPlayBtn.leadingAnchor constraintEqualToAnchor:urlField.trailingAnchor constant:8],
        [urlPlayBtn.trailingAnchor constraintEqualToAnchor:bar.trailingAnchor constant:-10],
        [urlPlayBtn.centerYAnchor constraintEqualToAnchor:urlField.centerYAnchor],
        [urlPlayBtn.heightAnchor constraintEqualToConstant:36],
        [urlPlayBtn.widthAnchor constraintGreaterThanOrEqualToConstant:72],

        [urlField.bottomAnchor constraintEqualToAnchor:bar.bottomAnchor constant:-10],
    ]];

    self.controlsVisible = YES;
    self.cRender.quality = SCRenderQualityBalanced;
    self.cRender.msaaLevel = SCRenderMSAA4x;
    [self updateModeButtonTitle];
}

- (BOOL)textFieldShouldReturn:(UITextField *)textField {
    if (textField == self.urlField) {
        [self playURLFromField];
        return NO;
    }
    return YES;
}

-(void)toggleControlsVisibility{
    self.controlsVisible = !self.controlsVisible;
    [UIView animateWithDuration:0.2 animations:^{
        self.controlsBar.alpha = self.controlsVisible ? 1.0 : 0.0;
    }];
    self.controlsBar.userInteractionEnabled = self.controlsVisible;
}

-(void)onCenterTap:(UITapGestureRecognizer *)gr{
    CGPoint p = [gr locationInView:self.view];
    CGRect bounds = self.view.bounds;
    /* 屏幕中心区：宽高各约 40% */
    CGFloat w = bounds.size.width * 0.4;
    CGFloat h = bounds.size.height * 0.4;
    CGRect center = CGRectMake((bounds.size.width - w) * 0.5,
                               (bounds.size.height - h) * 0.5,
                               w, h);
    if (!CGRectContainsPoint(center, p)) {
        return;
    }
    /* 控件显示且点在控件上时，交给按钮，不切换 */
    if (self.controlsVisible && CGRectContainsPoint(self.controlsBar.frame, p)) {
        return;
    }
    [self toggleControlsVisibility];
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
    self.view.backgroundColor = [UIColor colorWithRed:0.07 green:0.08 blue:0.10 alpha:1.0];

    [self.view insertSubview:self.cRender atIndex:0];
    [self buildControls];

    UITapGestureRecognizer *tap = [[UITapGestureRecognizer alloc] initWithTarget:self
                                                                        action:@selector(onCenterTap:)];
    tap.cancelsTouchesInView = NO;
    [self.view addGestureRecognizer:tap];
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    self.cRender.frame = self.view.bounds;
}

-(SCRender *)cRender{
    if(!_cRender){
        _cRender = [[SCRender alloc] initWithFrame:[UIScreen mainScreen].bounds];
        _cRender.fillMode = SCRenderFillModeAspectFit;
        _cRender.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    }
    return _cRender;
}

- (BOOL)prefersStatusBarHidden {
    return YES;
}

@end
