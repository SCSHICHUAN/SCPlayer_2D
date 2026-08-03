//
//  OCRender.m
//  SCFFmpeg
//
//  Created by stan on 2024/8/2.
//  Copyright © 2024 石川. All rights reserved.
//

#import "OCRender.h"
#import <OpenGLES/ES3/glext.h>
#import <GLKit/GLKit.h>
#import <AVFoundation/AVFoundation.h>
#import <VideoToolbox/VideoToolbox.h>


#define kDivisionNum 80

#define kLimitDegreeUpDown 80


typedef struct{
    GLfloat position[3];
    GLfloat texturePosition[2];
} Vertex;



@interface OCRender ()

@property(nonatomic,assign)GLfloat degreeX;
@property(nonatomic,assign)GLfloat degreeY;


@end

@implementation OCRender{
    
    EAGLContext * _context;
    CAEAGLLayer * _glLayer;
    
    GLuint _renderBuffer;
    GLuint _frameBuffer;
    GLuint _depthRenderBuffer;
    
    GLuint _program;
    
    GLuint _myPositionSlot;
    GLuint _myTextureCoordsSlot;
    GLuint _textureY;
    GLuint _textureU;
    GLuint _textureV;
    GLuint _myModelViewSlot;
    GLuint _myProjectionSlot;
    
    GLKMatrix4 _projectionMat;
    GLKMatrix4 _modelViewMat;
    
    CADisplayLink * _link;
    
    
    AVAsset * _myAsset;
    AVPlayerItem * _myPlayerItem;
    AVPlayer * _myPlyaer;
    AVPlayerItemVideoOutput * _myPlayerOutput;
    
    // 纹理缓存
    CVOpenGLESTextureCacheRef _cache;
    // 生成纹理Y
    CVOpenGLESTextureRef _lumaTexture;
    // 生成纹理UV
    CVOpenGLESTextureRef _chromaTexture;
}

-(instancetype)initWithFrame:(CGRect)frame{
    self = [super initWithFrame:frame];
    if(self){
        [self config];
    }
    return self;
}



- (void)config {
    
    [self setupGLLayerAndContext];
    [self setupRenderAndFrameBuffer];
    [self compiledShaderAndlinkProgram];
    [self setupVertexVBO];
//    [self setupPlayerData];
    [self setupPerspective];
    
    
    glClearColor(0.0, 0.0, 0.0, 1.0);
    
    // 面剔除以提高性能
    glEnable(GL_CULL_FACE); // 开启面剔除
    glCullFace(GL_BACK); // 剔除背面
    glFrontFace(GL_CW); // 设置顺时针为前面
    
    CVOpenGLESTextureCacheCreate(kCFAllocatorDefault, nil, _context, nil, &_cache);
    
    // 设置纹理关联关系
    glUniform1i(_textureY, 0);
    glUniform1i(_textureU, 1);
    glUniform1i(_textureV, 2);
    
//    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(toReplay) name:AVPlayerItemDidPlayToEndTimeNotification object:nil];
//    
//    _link = [CADisplayLink displayLinkWithTarget:self selector:@selector(render)];
//    [_link addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
}


- (void)toReplay{
    
    [_myPlyaer seekToTime:kCMTimeZero];
    [_myPlyaer play];
}

- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event{
    
    UITouch * touch = touches.anyObject;
    
    CGPoint currentPoint = [touch locationInView:self];
    CGPoint previousPoint = [touch previousLocationInView:self];
    
    self.degreeX += (previousPoint.x - currentPoint.x) * 0.5;
    self.degreeY += (previousPoint.y - currentPoint.y) * 0.5;
    
    // 限制上下转动的角度
    if (self.degreeY > kLimitDegreeUpDown) {
        self.degreeY = kLimitDegreeUpDown;
    }
    
    if (self.degreeY < -kLimitDegreeUpDown) {
        self.degreeY = -kLimitDegreeUpDown;
    }
}


/**
 设置glLayer和context
 */
- (void)setupGLLayerAndContext {
    
    _glLayer = [[CAEAGLLayer alloc] init];
    _glLayer.frame = self.frame;
    _glLayer.opaque = YES;
    _glLayer.drawableProperties = [NSDictionary dictionaryWithObjectsAndKeys:@(NO), kEAGLDrawablePropertyRetainedBacking, kEAGLColorFormatRGBA8, kEAGLDrawablePropertyColorFormat, nil];
    [self.layer addSublayer:_glLayer];
    
    _context = [[EAGLContext alloc] initWithAPI:kEAGLRenderingAPIOpenGLES2];
    if (!_context) {
        NSLog(@"context 创建失败");
    }
    if (![EAGLContext setCurrentContext:_context]) {
        NSLog(@"设置当前context失败");
    }
}



/**
 设置渲染缓存和帧缓存
 */
- (void)setupRenderAndFrameBuffer {
    // 开启深度测试
    glEnable(GL_DEPTH_TEST);
    // 申请渲染缓存
    glGenRenderbuffers(1, &_renderBuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, _renderBuffer);
    [_context renderbufferStorage:GL_RENDERBUFFER fromDrawable:_glLayer];
    
    // 获取缓存大小
    GLint with;
    GLint height;
    glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, &with);
    glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT, &height);
    // 申请深度测试缓存
    glGenRenderbuffers(1, &_depthRenderBuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, _depthRenderBuffer);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, with, height);
    // 设置帧缓存
    glGenFramebuffers(1, &_frameBuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, _frameBuffer);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, _renderBuffer);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, _depthRenderBuffer);
    // 设置回原来的渲染缓存
    glBindRenderbuffer(GL_RENDERBUFFER, _renderBuffer);
}


/**
 设置VBO
 */
- (void)setupVertexVBO {
    
    // 生成顶点数据（包括纹理顶点）
    Vertex * vertex = [self getBallDevidNum:kDivisionNum];
    // 生成顶点数据对应的索引数据
    GLuint * indexes = [self getBallVertexIndex:kDivisionNum];
    
    // 设置顶点数据的VBO缓存
    GLuint vertexBufferVBO;
    glGenBuffers(1, &vertexBufferVBO);
    glBindBuffer(GL_ARRAY_BUFFER, vertexBufferVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(Vertex) * (kDivisionNum + 1) * (kDivisionNum / 2 + 1), vertex, GL_STATIC_DRAW);
    
    // 设置顶点索引数据的VBO缓存
    GLuint indexBufferVBO;
    glGenBuffers(1, &indexBufferVBO);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indexBufferVBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(GLuint) * (kDivisionNum + 1) * (kDivisionNum + 1), indexes, GL_STATIC_DRAW);
    
    // 设置顶点数据在从VBO中读取和传递的指针设置
    glVertexAttribPointer(_myPositionSlot, 3, GL_FLOAT, GL_FALSE, sizeof(GLfloat) * 5, (GLvoid *)NULL);
    glEnableVertexAttribArray(_myPositionSlot);
    
    glVertexAttribPointer(_myTextureCoordsSlot, 2, GL_FLOAT, GL_FALSE, sizeof(GLfloat) * 5, (GLfloat *)NULL + 3);
    glEnableVertexAttribArray(_myTextureCoordsSlot);
    // 顶点数据已传给VBO可以释放原顶点数据
    free(vertex);
    free(indexes);
}



/**
 设置播放数据
 */
//- (void)setupPlayerData{
//
//    NSString * path = [[NSBundle mainBundle] pathForResource:@"demo.mp4" ofType:nil];
//    // 获取视频资源信息
//    _myAsset = [AVAsset assetWithURL:[NSURL fileURLWithPath:path]];
//
//    _myPlayerItem = [[AVPlayerItem alloc] initWithAsset:_myAsset];
//    // 创建视频播放器
//    _myPlyaer = [[AVPlayer alloc] initWithPlayerItem:_myPlayerItem];
//    // 播放视频
//    [_myPlyaer play];
//
//    // 设置视频格式信息
//    NSDictionary * dic = [NSDictionary dictionaryWithObjectsAndKeys:@(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange), kCVPixelBufferPixelFormatTypeKey, nil];
//    // 创建视频输出，后面会从_myPlayerOutput里读取视频的每一帧图像信息
//    _myPlayerOutput = [[AVPlayerItemVideoOutput alloc] initWithPixelBufferAttributes:dic];
//
//    [_myPlayerItem addOutput:_myPlayerOutput];
//}


/**
 设置视角
 */
- (void)setupPerspective{
    
    GLfloat aspect = self.frame.size.width / self.frame.size.height;
    
    _projectionMat = GLKMatrix4MakePerspective(GLKMathDegreesToRadians(60), aspect, 0.1, 100.f);
    glUniformMatrix4fv(_myProjectionSlot, 1, GL_FALSE, (GLfloat *)&_projectionMat.m);
}


/**
 渲染
 */
- (void)displayWithFrame:(AVFrame *)yuvFrame bb:(void (^)(BOOL success))completionBlock {
    
    dispatch_async(dispatch_get_main_queue(), ^{
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
       
        [self setupVideoTexture:yuvFrame];
        if(completionBlock){
            completionBlock(YES);
        }
        
        [self setupPerspective];
        self->_modelViewMat = GLKMatrix4RotateX(GLKMatrix4Identity, GLKMathDegreesToRadians(self.degreeY));
        self->_modelViewMat = GLKMatrix4RotateY(self->_modelViewMat, GLKMathDegreesToRadians(self.degreeX));
        glUniformMatrix4fv(self->_myModelViewSlot, 1, GL_FALSE, (GLfloat *)&self->_modelViewMat.m);
        glViewport(0, 0, self.frame.size.width, self.frame.size.height);

        glDrawElements(GL_TRIANGLE_STRIP, (kDivisionNum + 1) * (kDivisionNum + 1), GL_UNSIGNED_INT, 0);
//        glDrawArrays(GL_LINE_LOOP, 0, (kDivisionNum + 1) * (kDivisionNum / 2 + 1));
        [self->_context presentRenderbuffer:GL_RENDERBUFFER];
    });
    
   
   
   
}



/**
 设置视频数据转纹理
 */
- (void)setupVideoTexture:(AVFrame *)frame {
    if (frame == NULL) {
        return;
    }
    
    // 获取宽度和高度
    GLsizei textureWidth = frame->width;
    GLsizei textureHeight = frame->height;
    GLsizei halfWidth = textureWidth / 2;
    GLsizei halfHeight = textureHeight / 2;
    
    if (_cache == nil) {
        NSLog(@"no video texture cache");
    }
    
    // 销毁之前的纹理
    //    if (_lumaTexture) {
    //        CFRelease(_lumaTexture);
    //        _lumaTexture = nil;
    //    }
    //    if (_chromaUTexture) {
    //        CFRelease(_chromaUTexture);
    //        _chromaUTexture = nil;
    //    }
    //    if (_chromaVTexture) {
    //        CFRelease(_chromaVTexture);
    //        _chromaVTexture = nil;
    //    }
    
    // 刷新缓存区
    CVOpenGLESTextureCacheFlush(_cache, 0);
    
    // 绑定Y平面
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);//绑定位置 0
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, textureWidth, textureHeight, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, frame->data[0]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    
    
    //绑定U平面
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 1);//绑定位置 1
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, halfWidth, halfHeight, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, frame->data[1]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
   
    
    // 绑定V平面
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, 2);//绑定位置 2
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, halfWidth, halfHeight, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, frame->data[2]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    //设置uniforms并执行渲染
}



- (void)compiledShaderAndlinkProgram{
    
    GLuint vertexShader = [self loadShader:GL_VERTEX_SHADER withFileName:@"vertexShader.glsl"];
    GLuint fragmentShader = [self loadShader:GL_FRAGMENT_SHADER withFileName:@"fragmentShader.glsl"];
    
    _program = glCreateProgram();
    glAttachShader(_program, vertexShader);
    glAttachShader(_program, fragmentShader);
    glLinkProgram(_program);
    
    GLint linked = GL_FALSE;
    glGetProgramiv(_program, GL_LINK_STATUS, &linked);
    if (linked == GL_FALSE) {
        
        GLint infoLen;
        glGetProgramiv(_program, GL_INFO_LOG_LENGTH, &infoLen);
        if (infoLen > 0) {
            
            GLchar * info = malloc(sizeof(GLchar) * infoLen);
            glGetProgramInfoLog(_program, sizeof(GLchar) * infoLen, &infoLen, info);
            NSLog(@"%s", info);
            free(info);
        }
        glDeleteProgram(_program);
        _program = 0;
        return;
    }
    
    glUseProgram(_program);
    
    _myPositionSlot = glGetAttribLocation(_program, "myPosition");
    _myTextureCoordsSlot = glGetAttribLocation(_program, "myTextureCoordsIn");
    _myModelViewSlot = glGetUniformLocation(_program, "modelView");
    _myProjectionSlot = glGetUniformLocation(_program, "projection");
    
    
    _textureY = glGetUniformLocation(_program, "planY");
    _textureU = glGetUniformLocation(_program, "planU");
    _textureV = glGetUniformLocation(_program, "planV");
}


- (GLuint)loadShader:(GLenum)type withFileName:(NSString *)fileName{
    
    NSString * path = [[NSBundle mainBundle] pathForResource:fileName ofType:nil];
    
    NSError * error;
    NSString * shaderString = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:&error];
    
    const GLchar * cString = shaderString.UTF8String;
    
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &cString, NULL);
    glCompileShader(shader);
    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == GL_FALSE) {
        
        GLint infoLen = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &infoLen);
        if (infoLen > 0) {
            
            GLchar * info = malloc(sizeof(GLchar) * infoLen);
            
            glGetShaderInfoLog(shader, sizeof(GLchar) * infoLen, &infoLen, info);
            NSLog(@"%s", info);
            free(info);
        }
        glDeleteShader(shader);
        return 0;
    }
    
    return shader;
}



/**
 绘制一个球的顶点
 
 @param num 传入要生成的顶点的一层的个数（最后生成的顶点个数为 num * num）
 @return 返回生成后的顶点
 */
- (Vertex *)getBallDevidNum:(GLint) num{
    
    if (num % 2 == 1) {
        return 0;
    }
    
    GLfloat delta = 2 * M_PI / num; // 分割的份数
    GLfloat ballRaduis = 0.3; // 球的半径
    GLfloat pointZ;
    GLfloat pointX;
    GLfloat pointY;
    GLfloat textureY;
    GLfloat textureX;
    GLfloat textureYdelta = 1.0 / (num / 2);
    GLfloat textureXdelta = 1.0 / num;
    GLint layerNum = num / 2.0 + 1; // 层数
    GLint perLayerNum = num + 1; // 要让点再加到起点所以num + 1
    
    Vertex * cirleVertex = malloc(sizeof(Vertex) * perLayerNum * layerNum);
    memset(cirleVertex, 0x00, sizeof(Vertex) * perLayerNum * layerNum);
    
    // 层数
    for (int i = 0; i < layerNum; i++) {
        // 每层的高度(即pointY)，为负数让其从下向上创建
        pointY = -ballRaduis * cos(delta * i);
        
        // 每层的半径
        GLfloat layerRaduis = ballRaduis * sin(delta * i);
        // 每层圆的点,
        for (int j = 0; j < perLayerNum; j++) {
            // 计算
            pointX = layerRaduis * cos(delta * j);
            pointZ = layerRaduis * sin(delta * j);
            textureX = textureXdelta * j;
            //            textureY = textureYdelta * i;
            // 解决图片上下颠倒的问题
            textureY = 1 - textureYdelta * i;
            
            cirleVertex[i * perLayerNum + j] = (Vertex){pointX, pointY, pointZ, textureX, textureY};
        }
    }
    
    return cirleVertex;
}



- (GLuint *)getBallVertexIndex:(GLint)num{
    
    // 每层要多原点两次
    GLint sizeNum = sizeof(GLuint) * (num + 1) * (num + 1);
    
    GLuint * ballVertexIndex = malloc(sizeNum);
    memset(ballVertexIndex, 0x00, sizeNum);
    GLint layerNum = num / 2 + 1;
    GLint perLayerNum = num + 1; // 要让点再加到起点所以num + 1
    
    for (int i = 0; i < layerNum; i++) {
        
        if (i + 1 < layerNum) {
            
            for (int j = 0; j < perLayerNum; j++) {
                
                // i * perLayerNum * 2每层的下标是原来的2倍
                ballVertexIndex[(i * perLayerNum * 2) + (j * 2)] = i * perLayerNum + j;
                // 后一层数据
                ballVertexIndex[(i * perLayerNum * 2) + (j * 2 + 1)] = (i + 1) * perLayerNum + j;
            }
        } else {
            
            for (int j = 0; j < perLayerNum; j++) {
                // 后最一层数据单独处理
                ballVertexIndex[i * perLayerNum * 2 + j] = i * perLayerNum + j;
            }
        }
    }
    return ballVertexIndex;
}




- (void)dealloc{
    
    CFRelease(_cache);
    CFRelease(_lumaTexture);
    CFRelease(_chromaTexture);
}

@end



