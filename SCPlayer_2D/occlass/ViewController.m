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
#import <PhotosUI/PhotosUI.h>
#import "SCRender.h"
#include "SCPlayer.h"
#import "SCAudioQueuePlayer.h"
#import "SCDropdownButton.h"
#import "SCPlayer_audio.h"

static NSString * const kSCLastPlayURLKey = @"SCPlayer.lastPlayURL";

@interface ViewController () <UIImagePickerControllerDelegate, UINavigationControllerDelegate, UITextFieldDelegate, PHPickerViewControllerDelegate>
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
@property(nonatomic,strong)UIButton *videoPauseBtn;
@property(nonatomic,strong)UIButton *audioPauseBtn;
@property(nonatomic,strong)UIButton *avPauseBtn;
@property(nonatomic,strong)UIButton *rotateBtn;
@property(nonatomic,assign)int userRotateDegrees; /* 0=竖屏显示，90=横屏显示（叠加流元数据旋转） */
@property(nonatomic,assign)BOOL audioPaused;
@property(nonatomic,strong)UIView *controlsBar;
@property(nonatomic,assign)BOOL controlsVisible;
@property(nonatomic,assign)SCPlayer *playingIs;
@property(nonatomic,strong)UITextView *diffTextView;
@property(nonatomic,strong)NSMutableArray<NSString *> *diffLines;
-(void)initAudio:(void *)opaque;
-(void)startPlayWithPath:(NSString *)path;
-(void)playURLFromField;
-(BOOL)isNetworkURL:(NSString *)s;
-(void)stopCurrentPlayback;
-(void)toggleRenderMode;
-(void)updateModeButtonTitle;
-(void)toggleOrientRotate;
-(void)updateRotateButtonTitle;
-(void)applyCombinedRotate;
-(void)toggleVideoPause;
-(void)updateVideoPauseButtonTitle;
-(void)toggleAudioPause;
-(void)updateAudioPauseButtonTitle;
-(void)toggleAVPause;
-(void)updateAVPauseButtonTitle;
-(void)updateAllPauseButtonTitles;
-(UIButton *)makeButton:(NSString *)title action:(SEL)action;
-(void)layoutEqualWidthViews:(NSArray<UIView *> *)views
                       inBar:(UIView *)bar
                         top:(NSLayoutYAxisAnchor *)topAnchor
                 topConstant:(CGFloat)topConstant;
-(CGFloat)fittingWidthForButtonTitle:(NSString *)title;
-(void)buildControls;
-(void)buildDiffTextView;
-(void)appendDiffLog:(double)diffMs displayMs:(double)displayMs
             videoMB:(double)videoMB audioMB:(double)audioMB;
-(void)clearDiffLog;
-(void)toggleControlsVisibility;
-(void)onCenterTap:(UITapGestureRecognizer *)gr;
-(NSString *)albumVideosDirectory;
-(void)clearCopiedAlbumVideos;
-(void)pickAlbumVideoWithImagePicker;
-(void)importAndPlayAlbumVideoAtURL:(NSURL *)mediaURL;
@end

@implementation ViewController


//音频设备要数据
int audion_queue_call_other(void *userData, int flag, int len,
                            uint8_t **outPCM, int *outSize) {
    SCPlayer *scp = (SCPlayer *)userData;
    if (!scp) {
        return -1;
    }
    if (flag == 0) {
        /* 解码取 PCM */
        if (scp->audioq.nb_packets <= 0) {
            return 1; /* 暂无：上层填静音 */
        }
        audio_decode_callback(scp, NULL, 0);//需要数据
        if (!scp->audio_buf || scp->out_audio_size <= 0) {
            return -1; /* 结束 */
        }
        if (outPCM) {
            *outPCM = scp->audio_buf;//音频数据赋值
        }
        if (outSize) {
            *outSize = scp->out_audio_size;//赋值
        }
        return 0;
    }
    if (flag == 1) {
        /* 已不再用于水位；保留分支兼容 */
        return 0;
    }
    if (flag == 2) {
        audio_queue_wrote(scp, len); /* 入队 + 对齐上一盒 + 开本盒插值 */
        return 0;
    }
    return 0;
}

//渲染设备推送视频
int when_frame_push(AVFrame *frame, int flag, void *opaque, void *userData){
    ViewController *vc = (__bridge ViewController *)userData;
    SCPlayer *scp = (SCPlayer *)opaque;
    
    if(flag == 0){
        dispatch_async(dispatch_get_main_queue(), ^{
            [vc initAudio:opaque];
        });
    }else if(flag == 1){
        /* 模块已占 display_busy；接入方只负责：释放拷贝 + 清 busy */
        if (!frame) {
            if (scp) {
                scp->display_busy = 0;
            }
            return 0;
        }
        if (!scp || scp->quit) {
            av_frame_free(&frame);
            if (scp) {
                scp->display_busy = 0;
            }
            return 0;
        }
        vc.playingIs = scp;
        /* 流元数据旋转 + 用户横/竖屏偏好，避免每帧覆盖手动旋转 */
        int deg = (scp->video_rotate + vc.userRotateDegrees) % 360;
        if (deg < 0) deg += 360;
        vc.cRender.rotateDegrees = deg;
        double diffMs = scp->delay_video_time;
        double videoMB = scp->videoq.size / (1024.0 * 1024.0);
        double audioMB = scp->audioq.size / (1024.0 * 1024.0);
        [vc.cRender displayWithFrame:frame bb:^(BOOL success) {
            (void)success;
            double displayMs = vc.cRender.lastDisplayMs;
            dispatch_async(dispatch_get_main_queue(), ^{
                if (vc.playingIs == scp) {
                    [vc appendDiffLog:diffMs displayMs:displayMs videoMB:videoMB audioMB:audioMB];
                }
            });
            AVFrame *owned = frame;
            av_frame_free(&owned);
            if (vc.playingIs == scp) {
                scp->display_busy = 0;
            }
        }];
    }
    
    //    printf("fram.size = %d,flat = %d \n",frame->pkt_size,flag);
    return 0;
}


-(void)initAudio:(void *)opaque{
    SCPlayer *scp = (SCPlayer *)opaque;
    self.playingIs = scp;
    if (!scp) {
        return;
    }
    /* 不可用局部变量：initialize 后 AudioQueue 仍回调 self，局部对象会被 ARC 释放导致野指针崩溃 */
    [self.audioPlayer stop];
    self.audioPlayer = [[SCAudioQueuePlayer alloc] init];
    self.audioPlayer.audion_queue_call_other = audion_queue_call_other;
    [self.audioPlayer initializeAudioQueueWithSampleRate:scp->audioInfo.freq
                                                channels:scp->audioInfo.channels
                                                userData:scp];
    [self.audioPlayer play];
    self.audioPaused = NO;
    [self updateAllPauseButtonTitles];
}

-(void)stopCurrentPlayback{
    SCPlayer *scp = self.playingIs;
    self.playingIs = NULL;
    self.audioPaused = NO;
    [self.audioPlayer stop];
    self.audioPlayer = nil;
    self.cRender.rotateDegrees = 0;
    [self.cRender clearDisplay];
    [self updateAllPauseButtonTitles];
    /* 停播后恢复系统自动锁屏 */
    [UIApplication sharedApplication].idleTimerDisabled = NO;
    if (scp) {
        scp->display_busy = 0;
        scplayer_stop(scp);
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
    [self clearDiffLog];
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

    /* 播放期间保持屏幕常亮 */
    [UIApplication sharedApplication].idleTimerDisabled = YES;
    
    __weak typeof(self) weakSelf = self;
    dispatch_async(dispatch_get_global_queue(0, 0), ^{
        __strong typeof(weakSelf) strongSelf = weakSelf;
        if (!strongSelf) {
            return;
        }
        NSString *pathCopy = path;
        int ret = scplayer([pathCopy UTF8String], when_frame_push, (__bridge void *)strongSelf);
        dispatch_async(dispatch_get_main_queue(), ^{
            if (ret != 0) {
                strongSelf.lab.text = @"打开失败，请检查路径或 URL";
            }
            /* 本路播放结束且没有换到别的片源时，恢复自动锁屏 */
            if ([strongSelf.playingPath isEqualToString:pathCopy]) {
                [UIApplication sharedApplication].idleTimerDisabled = NO;
            }
        });
    });
}

-(void)playURLFromField{
    NSString *url = [self.urlField.text stringByTrimmingCharactersInSet:
                     [NSCharacterSet whitespaceAndNewlineCharacterSet]];
    if (url.length > 0) {
        [[NSUserDefaults standardUserDefaults] setObject:url forKey:kSCLastPlayURLKey];
    }
    [self startPlayWithPath:url];
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

-(void)applyCombinedRotate{
    int base = self.playingIs ? self.playingIs->video_rotate : 0;
    int deg = (base + self.userRotateDegrees) % 360;
    if (deg < 0) deg += 360;
    self.cRender.rotateDegrees = deg;
}

-(void)toggleOrientRotate{
    /* 竖屏(0) ↔ 横屏(90) */
    self.userRotateDegrees = (self.userRotateDegrees == 0) ? 90 : 0;
    [self applyCombinedRotate];
    [self updateRotateButtonTitle];
    self.lab.text = (self.userRotateDegrees == 0) ? @"画面: 竖屏" : @"画面: 横屏";
}

-(void)updateRotateButtonTitle{
    /* 按钮显示当前可切换到的目标方向 */
    NSString *title = (self.userRotateDegrees == 0) ? @"横屏" : @"竖屏";
    [self.rotateBtn setTitle:title forState:UIControlStateNormal];
}

-(void)toggleVideoPause{
    SCPlayer *scp = self.playingIs;
    if (!scp) {
        self.lab.text = @"当前没有在播的视频";
        return;
    }
    /* 1=视频暂停（音频继续），0=视频播放 */
    scp->vidoe_stop = scp->vidoe_stop ? 0 : 1;
    [self updateAllPauseButtonTitles];
}

-(void)updateVideoPauseButtonTitle{
    SCPlayer *scp = self.playingIs;
    BOOL paused = (scp && scp->vidoe_stop == 1);
    [self.videoPauseBtn setTitle:(paused ? @"V播放" : @"V暂停") forState:UIControlStateNormal];
}

-(void)toggleAudioPause{
    if (!self.playingIs || !self.audioPlayer) {
        self.lab.text = @"当前没有在播的音频";
        return;
    }
    if (self.audioPaused) {
        [self.audioPlayer resume];
        self.audioPaused = NO;
        audio_aq_set_paused(self.playingIs, 0);
    } else {
        [self.audioPlayer pause];
        self.audioPaused = YES;
        audio_aq_set_paused(self.playingIs, 1);
    }
    [self updateAllPauseButtonTitles];
}

-(void)updateAudioPauseButtonTitle{
    [self.audioPauseBtn setTitle:(self.audioPaused ? @"A播放" : @"A暂停")
                        forState:UIControlStateNormal];
}

-(void)toggleAVPause{
    SCPlayer *scp = self.playingIs;
    if (!scp) {
        self.lab.text = @"当前没有在播的内容";
        return;
    }
    BOOL paused = (scp->vidoe_stop == 1) && self.audioPaused;
    if (paused) {
        /* 同步恢复音视频 */
        scp->vidoe_stop = 0;
        if (self.audioPlayer) {
            [self.audioPlayer resume];
        }
        self.audioPaused = NO;
        audio_aq_set_paused(scp, 0);
    } else {
        /* 同步暂停音视频 */
        scp->vidoe_stop = 1;
        if (self.audioPlayer) {
            [self.audioPlayer pause];
        }
        self.audioPaused = YES;
        audio_aq_set_paused(scp, 1);
    }
    [self updateAllPauseButtonTitles];
}

-(void)updateAVPauseButtonTitle{
    SCPlayer *scp = self.playingIs;
    BOOL paused = (scp && scp->vidoe_stop == 1 && self.audioPaused);
    [self.avPauseBtn setTitle:(paused ? @"播放" : @"暂停") forState:UIControlStateNormal];
}

-(void)updateAllPauseButtonTitles{
    [self updateVideoPauseButtonTitle];
    [self updateAudioPauseButtonTitle];
    [self updateAVPauseButtonTitle];
}

/* 与 GL-ARKit makeButton 同风格：半透明黑底 + 白字 + 圆角 */
-(UIButton *)makeButton:(NSString *)title action:(SEL)action {
    UIButton *b = [UIButton buttonWithType:UIButtonTypeSystem];
    [b setTitle:title forState:UIControlStateNormal];
    b.titleLabel.font = [UIFont monospacedDigitSystemFontOfSize:14 weight:UIFontWeightSemibold];
    b.titleLabel.adjustsFontSizeToFitWidth = YES;
    b.titleLabel.minimumScaleFactor = 0.75;
    b.titleLabel.lineBreakMode = NSLineBreakByClipping;
    b.backgroundColor = [[UIColor blackColor] colorWithAlphaComponent:0.45];
    [b setTitleColor:UIColor.whiteColor forState:UIControlStateNormal];
    b.layer.cornerRadius = 8;
    b.contentEdgeInsets = UIEdgeInsetsMake(8, 10, 8, 10);
    /* 按文字固有宽度，禁止被挤扁截断 */
    [b setContentHuggingPriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
    [b setContentCompressionResistancePriority:UILayoutPriorityRequired
                                       forAxis:UILayoutConstraintAxisHorizontal];
    if (action) {
        [b addTarget:self action:action forControlEvents:UIControlEventTouchUpInside];
    }
    return b;
}

-(CGFloat)fittingWidthForButtonTitle:(NSString *)title {
    UIFont *font = [UIFont monospacedDigitSystemFontOfSize:14 weight:UIFontWeightSemibold];
    CGSize sz = [title sizeWithAttributes:@{NSFontAttributeName: font}];
    return ceil(sz.width) + 20.0; /* contentEdgeInsets 左右各 10 */
}

/* 一排最多 4 个：按文字固有宽度左对齐，不等宽、不拉满（布局位置稳定） */
-(void)layoutEqualWidthViews:(NSArray<UIView *> *)views
                       inBar:(UIView *)bar
                         top:(NSLayoutYAxisAnchor *)topAnchor
                 topConstant:(CGFloat)topConstant {
    NSUInteger n = views.count;
    NSAssert(n >= 1 && n <= 4, @"row supports 1..4 controls");
    UIView *prev = nil;
    for (NSUInteger i = 0; i < n; i++) {
        UIView *v = views[i];
        v.translatesAutoresizingMaskIntoConstraints = NO;
        [v setContentHuggingPriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
        [v setContentCompressionResistancePriority:UILayoutPriorityRequired
                                           forAxis:UILayoutConstraintAxisHorizontal];
        NSMutableArray<NSLayoutConstraint *> *cs = [NSMutableArray array];
        [cs addObject:[v.topAnchor constraintEqualToAnchor:topAnchor constant:topConstant]];
        [cs addObject:[v.heightAnchor constraintEqualToConstant:36]];
        if (i == 0) {
            [cs addObject:[v.leadingAnchor constraintEqualToAnchor:bar.leadingAnchor constant:10]];
        } else {
            [cs addObject:[v.leadingAnchor constraintEqualToAnchor:prev.trailingAnchor constant:8]];
        }
        /* 不贴右缘、不等宽，避免被拉伸；右边界软约束防止超出 bar */
        NSLayoutConstraint *cap = [v.trailingAnchor constraintLessThanOrEqualToAnchor:bar.trailingAnchor
                                                                             constant:-10];
        cap.priority = UILayoutPriorityDefaultHigh;
        [cs addObject:cap];
        [NSLayoutConstraint activateConstraints:cs];
        prev = v;
    }
}

-(NSString *)albumVideosDirectory {
    NSString *docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
    NSString *dir = [docs stringByAppendingPathComponent:@"AlbumVideos"];
    [[NSFileManager defaultManager] createDirectoryAtPath:dir
                              withIntermediateDirectories:YES
                                               attributes:nil
                                                    error:nil];
    return dir;
}

/* 选新相册视频前清掉本机已拷贝的旧文件 */
-(void)clearCopiedAlbumVideos {
    NSFileManager *fm = [NSFileManager defaultManager];
    NSString *dir = [self albumVideosDirectory];
    NSArray *files = [fm contentsOfDirectoryAtPath:dir error:nil];
    for (NSString *f in files) {
        [fm removeItemAtPath:[dir stringByAppendingPathComponent:f] error:nil];
    }
    [[NSUserDefaults standardUserDefaults] removeObjectForKey:@"SCPlayer.albumHistory"];
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
    /* 模式标题长短会变，固定为较长文案宽度，避免切换时整行左右晃 */
    CGFloat modeW = MAX([self fittingWidthForButtonTitle:@"等比例"],
                        [self fittingWidthForButtonTitle:@"拉伸铺满"]);
    [modeBtn.widthAnchor constraintEqualToConstant:modeW].active = YES;

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
    /* 系统 clear 按钮在深色底上看不见，改用自定义白色 × */
    urlField.clearButtonMode = UITextFieldViewModeNever;
    urlField.returnKeyType = UIReturnKeyGo;
    urlField.layer.cornerRadius = 8;
    urlField.leftView = [[UIView alloc] initWithFrame:CGRectMake(0, 0, 10, 1)];
    urlField.leftViewMode = UITextFieldViewModeAlways;
    UIButton *clearBtn = [UIButton buttonWithType:UIButtonTypeSystem];
    clearBtn.frame = CGRectMake(0, 0, 28, 28);
    if (@available(iOS 13.0, *)) {
        UIImage *img = [UIImage systemImageNamed:@"xmark.circle.fill"];
        [clearBtn setImage:img forState:UIControlStateNormal];
    } else {
        [clearBtn setTitle:@"✕" forState:UIControlStateNormal];
        clearBtn.titleLabel.font = [UIFont systemFontOfSize:14 weight:UIFontWeightSemibold];
    }
    clearBtn.tintColor = [UIColor colorWithWhite:1 alpha:0.85];
    [clearBtn setTitleColor:[UIColor colorWithWhite:1 alpha:0.85] forState:UIControlStateNormal];
    [clearBtn addTarget:self action:@selector(clearURLField) forControlEvents:UIControlEventTouchUpInside];
    urlField.rightView = clearBtn;
    urlField.rightViewMode = UITextFieldViewModeWhileEditing;
    urlField.delegate = self;
    self.urlField = urlField;

    NSString *savedURL = [[NSUserDefaults standardUserDefaults] stringForKey:kSCLastPlayURLKey];
    if (savedURL.length > 0) {
        urlField.text = savedURL;
    }

    UIButton *urlPlayBtn = [self makeButton:@"播放URL" action:@selector(playURLFromField)];
    urlPlayBtn.translatesAutoresizingMaskIntoConstraints = NO;

    UIButton *videoPauseBtn = [self makeButton:@"V暂停" action:@selector(toggleVideoPause)];
    self.videoPauseBtn = videoPauseBtn;

    UIButton *audioPauseBtn = [self makeButton:@"A暂停" action:@selector(toggleAudioPause)];
    self.audioPauseBtn = audioPauseBtn;

    UIButton *avPauseBtn = [self makeButton:@"暂停" action:@selector(toggleAVPause)];
    self.avPauseBtn = avPauseBtn;

    UIButton *rotateBtn = [self makeButton:@"横屏" action:@selector(toggleOrientRotate)];
    self.rotateBtn = rotateBtn;
    CGFloat rotateW = MAX([self fittingWidthForButtonTitle:@"横屏"],
                          [self fittingWidthForButtonTitle:@"竖屏"]);
    [rotateBtn.widthAnchor constraintEqualToConstant:rotateW].active = YES;

    [bar addSubview:urlField];
    [bar addSubview:urlPlayBtn];
    [bar addSubview:videoPauseBtn];
    [bar addSubview:audioPauseBtn];
    [bar addSubview:avPauseBtn];
    [bar addSubview:rotateBtn];

    UILayoutGuide *safe = self.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [bar.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:12],
        [bar.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-12],
        [bar.topAnchor constraintEqualToAnchor:safe.topAnchor constant:8],

        /* 第1行：提示信息 */
        [lab.leadingAnchor constraintEqualToAnchor:bar.leadingAnchor constant:10],
        [lab.trailingAnchor constraintEqualToAnchor:bar.trailingAnchor constant:-10],
        [lab.topAnchor constraintEqualToAnchor:bar.topAnchor constant:8],
    ]];

    /* 第2行：本地 / 相册 / 模式 / 画质 —— 按文字宽度，最多 4 */
    [self layoutEqualWidthViews:@[startBtn, albumBtn, modeBtn, qualityDrop]
                          inBar:bar
                            top:lab.bottomAnchor
                    topConstant:8];

    /* 第3行：抗锯齿 / 横竖屏 / V / A —— 按文字宽度，最多 4 */
    [self layoutEqualWidthViews:@[msaaDrop, rotateBtn, videoPauseBtn, audioPauseBtn]
                          inBar:bar
                            top:startBtn.bottomAnchor
                    topConstant:8];

    /* 第4行：同步暂停 */
    [self layoutEqualWidthViews:@[avPauseBtn]
                          inBar:bar
                            top:msaaDrop.bottomAnchor
                    topConstant:8];

    /* 第5行：URL + 播放 */
    [NSLayoutConstraint activateConstraints:@[
        [urlField.leadingAnchor constraintEqualToAnchor:bar.leadingAnchor constant:10],
        [urlField.topAnchor constraintEqualToAnchor:avPauseBtn.bottomAnchor constant:8],
        [urlField.heightAnchor constraintEqualToConstant:36],
        [urlField.bottomAnchor constraintEqualToAnchor:bar.bottomAnchor constant:-10],

        [urlPlayBtn.leadingAnchor constraintEqualToAnchor:urlField.trailingAnchor constant:8],
        [urlPlayBtn.trailingAnchor constraintEqualToAnchor:bar.trailingAnchor constant:-10],
        [urlPlayBtn.centerYAnchor constraintEqualToAnchor:urlField.centerYAnchor],
        [urlPlayBtn.heightAnchor constraintEqualToConstant:36],
        [urlPlayBtn.widthAnchor constraintEqualToConstant:88],
    ]];

    self.controlsVisible = YES;
    self.cRender.quality = SCRenderQualityBalanced;
    self.cRender.msaaLevel = SCRenderMSAA4x;
    self.userRotateDegrees = 0;
    [self updateModeButtonTitle];
    [self updateRotateButtonTitle];
    [self updateAllPauseButtonTitles];
    [self buildDiffTextView];
}

-(void)buildDiffTextView{
    UITextView *tv = [[UITextView alloc] initWithFrame:CGRectZero];
    tv.translatesAutoresizingMaskIntoConstraints = NO;
    tv.backgroundColor = [[UIColor blackColor] colorWithAlphaComponent:0.45];
    tv.textColor = [UIColor colorWithWhite:1 alpha:0.9];
    tv.font = [UIFont monospacedDigitSystemFontOfSize:12 weight:UIFontWeightRegular];
    tv.editable = NO;
    tv.selectable = NO;
    tv.scrollEnabled = YES;
    tv.showsVerticalScrollIndicator = YES;
    tv.textContainerInset = UIEdgeInsetsMake(8, 8, 8, 8);
    tv.layer.cornerRadius = 8;
    tv.clipsToBounds = YES;
    tv.text = @"";
    self.diffTextView = tv;
    self.diffLines = [NSMutableArray array];

    [self.view addSubview:tv];
    [self.view bringSubviewToFront:tv];
    UILayoutGuide *safe = self.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [tv.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:12],
        [tv.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-12],
        [tv.bottomAnchor constraintEqualToAnchor:safe.bottomAnchor constant:-12],
        [tv.heightAnchor constraintEqualToConstant:100],
    ]];
}

-(void)appendDiffLog:(double)diffMs displayMs:(double)displayMs
             videoMB:(double)videoMB audioMB:(double)audioMB{
    if (!self.diffTextView) {
        return;
    }
    if (!self.diffLines) {
        self.diffLines = [NSMutableArray array];
    }
    /* 精简一行：同步差 | 渲染耗时 | 音视频包缓存 */
    NSString *line = [NSString stringWithFormat:
                      @"diff:%.1fms display:%.1fms video:%.2fMB audio:%.2fMB",
                      diffMs, displayMs, videoMB, audioMB];
    [self.diffLines addObject:line];
    while (self.diffLines.count > 100) {
        [self.diffLines removeObjectAtIndex:0];
    }
    self.diffTextView.text = [self.diffLines componentsJoinedByString:@"\n"];
    NSUInteger len = self.diffTextView.text.length;
    if (len > 0) {
        [self.diffTextView scrollRangeToVisible:NSMakeRange(len - 1, 1)];
    }
}

-(void)clearDiffLog{
    [self.diffLines removeAllObjects];
    self.diffTextView.text = @"";
}

- (BOOL)textFieldShouldReturn:(UITextField *)textField {
    if (textField == self.urlField) {
        [self playURLFromField];
        return NO;
    }
    return YES;
}

-(void)clearURLField{
    self.urlField.text = @"";
}

-(void)toggleControlsVisibility{
    self.controlsVisible = !self.controlsVisible;
    [UIView animateWithDuration:0.2 animations:^{
        self.controlsBar.alpha = self.controlsVisible ? 1.0 : 0.0;
        self.diffTextView.alpha = self.controlsVisible ? 1.0 : 0.0;
    }];
    self.controlsBar.userInteractionEnabled = self.controlsVisible;
    self.diffTextView.userInteractionEnabled = self.controlsVisible;
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
    if (self.controlsVisible && CGRectContainsPoint(self.diffTextView.frame, p)) {
        return;
    }
    [self toggleControlsVisibility];
}

-(void)pickAlbumVideo{
    if (@available(iOS 14.0, *)) {
        /* PHPicker：点选即回调，无 UIImagePicker 的「使用视频」二次确认/剪辑页 */
        PHPickerConfiguration *config = [[PHPickerConfiguration alloc] init];
        config.filter = [PHPickerFilter videosFilter];
        config.selectionLimit = 1;
        config.preferredAssetRepresentationMode = PHPickerConfigurationAssetRepresentationModeCurrent;
        PHPickerViewController *picker = [[PHPickerViewController alloc] initWithConfiguration:config];
        picker.delegate = self;
        picker.modalPresentationStyle = UIModalPresentationFullScreen;
        [self presentViewController:picker animated:YES completion:nil];
        return;
    }
    [self pickAlbumVideoWithImagePicker];
}

-(void)pickAlbumVideoWithImagePicker{
    if (![UIImagePickerController isSourceTypeAvailable:UIImagePickerControllerSourceTypePhotoLibrary]) {
        self.lab.text = @"当前设备不支持相册";
        return;
    }
    UIImagePickerController *picker = [[UIImagePickerController alloc] init];
    picker.sourceType = UIImagePickerControllerSourceTypePhotoLibrary;
    picker.mediaTypes = @[(NSString *)kUTTypeMovie];
    picker.delegate = self;
    picker.allowsEditing = NO;
    if (@available(iOS 11.0, *)) {
        picker.videoExportPreset = AVAssetExportPresetPassthrough;
    }
    picker.modalPresentationStyle = UIModalPresentationFullScreen;
    [self presentViewController:picker animated:YES completion:nil];
}

/* 先清旧拷贝，再拷进 Documents/AlbumVideos 并播放 */
-(void)importAndPlayAlbumVideoAtURL:(NSURL *)mediaURL {
    if (!mediaURL) {
        self.lab.text = @"未获取到视频文件";
        return;
    }
    [self clearCopiedAlbumVideos];
    NSString *ext = mediaURL.pathExtension.length ? mediaURL.pathExtension : @"MOV";
    NSString *base = mediaURL.lastPathComponent.length
        ? mediaURL.lastPathComponent.stringByDeletingPathExtension
        : @"album";
    NSString *unique = [NSString stringWithFormat:@"%@_%@.%@",
                        base,
                        @((long long)([[NSDate date] timeIntervalSince1970] * 1000)),
                        ext];
    NSString *localPath = [[self albumVideosDirectory] stringByAppendingPathComponent:unique];
    NSError *err = nil;
    if (![[NSFileManager defaultManager] copyItemAtURL:mediaURL
                                                 toURL:[NSURL fileURLWithPath:localPath]
                                                 error:&err]) {
        self.lab.text = err.localizedDescription ?: @"拷贝相册视频失败";
        return;
    }
    [self startPlayWithPath:localPath];
}

#pragma mark - PHPickerViewControllerDelegate

- (void)picker:(PHPickerViewController *)picker didFinishPicking:(NSArray<PHPickerResult *> *)results API_AVAILABLE(ios(14)) {
    [picker dismissViewControllerAnimated:YES completion:nil];
    if (results.count == 0) {
        self.lab.text = @"已取消选择";
        return;
    }
    NSItemProvider *provider = results.firstObject.itemProvider;
    NSString *typeId = @"public.movie";
    if (![provider hasItemConformingToTypeIdentifier:typeId]) {
        /* 部分视频只声明具体 UTI */
        NSArray<NSString *> *fallbacks = @[
            @"public.mpeg-4",
            @"com.apple.quicktime-movie",
            @"public.avi",
        ];
        typeId = nil;
        for (NSString *t in fallbacks) {
            if ([provider hasItemConformingToTypeIdentifier:t]) {
                typeId = t;
                break;
            }
        }
    }
    if (!typeId) {
        self.lab.text = @"无法读取该视频";
        return;
    }
    self.lab.text = @"正在导入相册视频…";
    __weak typeof(self) weakSelf = self;
    [provider loadFileRepresentationForTypeIdentifier:typeId
                                     completionHandler:^(NSURL * _Nullable url, NSError * _Nullable error) {
        /* url 仅在本回调内有效，必须同步拷贝 */
        NSString *tmpCopy = nil;
        if (url && !error) {
            NSString *ext = url.pathExtension.length ? url.pathExtension : @"MOV";
            tmpCopy = [NSTemporaryDirectory() stringByAppendingPathComponent:
                       [NSString stringWithFormat:@"phpick_%@.%@", NSUUID.UUID.UUIDString, ext]];
            [[NSFileManager defaultManager] removeItemAtPath:tmpCopy error:nil];
            NSError *copyErr = nil;
            if (![[NSFileManager defaultManager] copyItemAtURL:url
                                                         toURL:[NSURL fileURLWithPath:tmpCopy]
                                                         error:&copyErr]) {
                tmpCopy = nil;
                error = copyErr;
            }
        }
        NSString *path = tmpCopy;
        NSError *errOut = error;
        dispatch_async(dispatch_get_main_queue(), ^{
            __strong typeof(weakSelf) strongSelf = weakSelf;
            if (!strongSelf) return;
            if (!path.length) {
                strongSelf.lab.text = errOut.localizedDescription ?: @"导入相册视频失败";
                return;
            }
            [strongSelf importAndPlayAlbumVideoAtURL:[NSURL fileURLWithPath:path]];
            [[NSFileManager defaultManager] removeItemAtPath:path error:nil];
        });
    }];
}

#pragma mark - UIImagePickerControllerDelegate

- (void)imagePickerController:(UIImagePickerController *)picker
didFinishPickingMediaWithInfo:(NSDictionary<UIImagePickerControllerInfoKey,id> *)info {
    NSURL *mediaURL = info[UIImagePickerControllerMediaURL];
    [picker dismissViewControllerAnimated:YES completion:^{
        [self importAndPlayAlbumVideoAtURL:mediaURL];
    }];
}

- (void)imagePickerControllerDidCancel:(UIImagePickerController *)picker {
    [picker dismissViewControllerAnimated:YES completion:nil];
    self.lab.text = @"已取消选择";
}



- (void)viewDidLoad {
    [super viewDidLoad];
    /* 进播前预热音频 session，避免冷启动首播爆破音（见 README §7） */
    [SCAudioQueuePlayer warmUpAudioSession];
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

- (void)viewWillDisappear:(BOOL)animated {
    [super viewWillDisappear:animated];
    if (self.isMovingFromParentViewController || self.isBeingDismissed) {
        [UIApplication sharedApplication].idleTimerDisabled = NO;
    }
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
