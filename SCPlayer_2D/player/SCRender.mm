//
//  SCRender.m
//  SCFFmpeg
//
//  Created by stan on 2024/1/17.
//  Copyright © 2024 石川. All rights reserved.
//

#import "SCRender.h"
#import <OpenGLES/ES3/glext.h>
#import <GLKit/GLKit.h>
#import <QuartzCore/QuartzCore.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#import "Camera.h"
//#import "Public.h"




#import "JpegUtil.h"

/* 毫秒：CACurrentMediaTime 单位是秒 */
static inline double sc_gl_now_ms(void) {
    return CACurrentMediaTime() * 1000.0;
}

// camera
Camera camera(glm::vec3(0.0f, 0.0f, 0.0f));




@interface SCRender ()
{
    GLKView *glView;
    dispatch_queue_t _display_rgb_queue;
    /// 顶点对象
    GLuint _VBO;
    GLuint _VAO;
    GLuint _yTexture;
    GLuint _uTexture;
    GLuint _vTexture;
    GLuint _glProgram;
    /// 顶点着色器
    GLuint _vertextShader;
    /// 片段着色器
    GLuint _fragmentShader;
    
    GLuint          _colorRenderBuffer;   /* drawable 解析目标 */
    GLuint          _depthRenderBuffer;   /* 非 MSAA 深度；MSAA 时用 _msaaDepth */
    GLuint          _frameBuffer;         /* 实际绘制用：MSAA 或 resolve */
    GLuint          _resolveFrameBuffer;  /* 挂 drawable 的 FBO */
    GLuint          _msaaFrameBuffer;
    GLuint          _msaaColorBuffer;
    GLuint          _msaaDepthBuffer;
    GLint           _fboWidth;
    GLint           _fboHeight;
    GLint           _msaaSamples; /* 0=关，2/4 */
    BOOL            _useMSAA;
    CAEAGLLayer     *_eaglLayer;
}
@end

@implementation SCRender
-(void)setForward:(float)forward{
    _forward = forward;
    camera.ProcessKeyboard(FORWARD, _forward/100000000.0);
}

-(void)setBack:(float)back{
    _back = back;
    camera.ProcessKeyboard(BACKWARD, _back/100000000.0);
}
-(void)setLeft:(float)left{
    _left = left;
    camera.ProcessKeyboard(LEFT, _back/100000000.0);
}
-(void)setRight:(float)right{
    _right = right;
    camera.ProcessKeyboard(RIGHT, _back/100000000.0);
    
}
-(void)setRight_R:(float)right_R{
    _right_R = right_R;
    camera.ProcessMouseMovement(_right_R/1000000.0, 0);
}


-(instancetype)initWithFrame:(CGRect)frame{
    self = [super initWithFrame:frame];
    if(self){
        _fillMode = SCRenderFillModeAspectFit;
        _quality = SCRenderQualityBalanced;
        _msaaLevel = SCRenderMSAA4x;
        _rotateDegrees = 0;
        [self config];
    }
    return self;
}

-(void)config{
    [self _creatOpenGLContent];
    [self _setupOpenGLProgram];
    [self _setupOpenGL];
}

- (CGFloat)_scaleForQuality {
    CGFloat native = [UIScreen mainScreen].scale;
    if (native < 1.0) {
        native = 1.0;
    }
    switch (self.quality) {
        case SCRenderQualityFluent:
            return 1.0;
        case SCRenderQualityHigh:
            return native;
        case SCRenderQualityUltra:
            /* 高于屏密度超采样，上限 4 防止 FBO 过大 */
            return MIN(native * 1.5, 4.0);
        case SCRenderQualityBalanced:
        default:
            return MIN(2.0, native);
    }
}

- (GLint)_samplesForMSAALevel {
    GLint want = 0;
    switch (self.msaaLevel) {
        case SCRenderMSAA2x: want = 2; break;
        case SCRenderMSAA4x: want = 4; break;
        case SCRenderMSAA8x: want = 8; break;
        case SCRenderMSAAOff:
        default: want = 0; break;
    }
    if (want <= 0) {
        return 0;
    }
    GLint maxSamples = 0;
    glGetIntegerv(GL_MAX_SAMPLES, &maxSamples);
    if (maxSamples < 1) {
        maxSamples = 4;
    }
    return MIN(want, maxSamples);
}

- (void)setQuality:(SCRenderQuality)quality {
    if (_quality == quality) {
        return;
    }
    _quality = quality;
    [self applyDrawableScaleAndRebuild];
}

- (void)setMsaaLevel:(SCRenderMSAALevel)msaaLevel {
    if (_msaaLevel == msaaLevel) {
        return;
    }
    _msaaLevel = msaaLevel;
    [self applyDrawableScaleAndRebuild];
}

- (void)destroyFrameAndRenderBuffer {
    if (!glView.context) {
        return;
    }
    [EAGLContext setCurrentContext:glView.context];
    if (_msaaColorBuffer) { glDeleteRenderbuffers(1, &_msaaColorBuffer); _msaaColorBuffer = 0; }
    if (_msaaDepthBuffer) { glDeleteRenderbuffers(1, &_msaaDepthBuffer); _msaaDepthBuffer = 0; }
    if (_msaaFrameBuffer) { glDeleteFramebuffers(1, &_msaaFrameBuffer); _msaaFrameBuffer = 0; }
    if (_depthRenderBuffer) { glDeleteRenderbuffers(1, &_depthRenderBuffer); _depthRenderBuffer = 0; }
    if (_colorRenderBuffer) { glDeleteRenderbuffers(1, &_colorRenderBuffer); _colorRenderBuffer = 0; }
    if (_resolveFrameBuffer) { glDeleteFramebuffers(1, &_resolveFrameBuffer); _resolveFrameBuffer = 0; }
    _frameBuffer = 0;
    _useMSAA = NO;
    _msaaSamples = 0;
    _fboWidth = _fboHeight = 0;
}

- (void)applyDrawableScaleAndRebuild {
    if (!glView || !_eaglLayer) {
        return;
    }
    CGFloat scale = [self _scaleForQuality];
    glView.contentScaleFactor = scale;
    _eaglLayer.contentsScale = scale;
    [self destroyFrameAndRenderBuffer];
    [self setupFrameAndRenderBuffer];
}

-(void)_creatOpenGLContent{
    glView = [[GLKView alloc] initWithFrame:self.bounds];
    glView.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    glView.context = [[EAGLContext alloc] initWithAPI:kEAGLRenderingAPIOpenGLES3];
    /* 自管 FBO，关掉 GLKView 自带 drawable，避免抢 layer */
    glView.enableSetNeedsDisplay = NO;
    [EAGLContext setCurrentContext:glView.context];
    [self addSubview:glView];
    _eaglLayer = (CAEAGLLayer *)glView.layer;
    _eaglLayer.opaque = YES;
    _eaglLayer.drawableProperties = @{
        kEAGLDrawablePropertyRetainedBacking : @NO,
        kEAGLDrawablePropertyColorFormat : kEAGLColorFormatRGBA8
    };
    [self applyDrawableScaleAndRebuild];
}

- (void)layoutSubviews {
    [super layoutSubviews];
    if (!glView) {
        return;
    }
    CGSize old = glView.bounds.size;
    glView.frame = self.bounds;
    CGSize now = glView.bounds.size;
    if (fabs(old.width - now.width) > 0.5 || fabs(old.height - now.height) > 0.5) {
        [self applyDrawableScaleAndRebuild];
    }
}


#pragma mark - OpenGL
/// 编译着色器
- (GLuint)_compileShader:(NSString *)shaderName shaderType:(GLuint)shaderType {
    if(shaderName.length == 0) return -1;
    NSString *shaderPath = [[NSBundle mainBundle] pathForResource:shaderName ofType:@"glsl"];
    NSError *error;
    NSString *source = [NSString stringWithContentsOfFile:shaderPath encoding:NSUTF8StringEncoding error:&error];
    if(error) return -1;
    GLuint shader = glCreateShader(shaderType);
    const char *ss = [source UTF8String];
    glShaderSource(shader, 1, &ss, NULL);
    glCompileShader(shader);
    int  success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if(!success) {
        char infoLog[512];
        glGetShaderInfoLog(shader, 512, NULL, infoLog);
        printf("shader error msg: %s \n", infoLog);
    }
    return shader;
}
/// 初始化OpenGL可编程程序
- (BOOL)_setupOpenGLProgram {
    //    [self.openGLContext makeCurrentContext];
    [EAGLContext setCurrentContext:glView.context];
    _glProgram = glCreateProgram();
    _vertextShader = [self _compileShader:@"vertex" shaderType:GL_VERTEX_SHADER];
    _fragmentShader = [self _compileShader:@"yuv_fragment" shaderType:GL_FRAGMENT_SHADER];
    glAttachShader(_glProgram, _vertextShader);
    glAttachShader(_glProgram, _fragmentShader);
    glLinkProgram(_glProgram);
    GLint success;
    glGetProgramiv(_glProgram, GL_LINK_STATUS, &success);
    if(!success) {
        char infoLog[512];
        glGetProgramInfoLog(_glProgram, 512, NULL, infoLog);
        printf("Link shader error: %s \n", infoLog);
    }
    glDeleteShader(_vertextShader);
    glDeleteShader(_fragmentShader);
    NSLog(@"===着色器加载成功===");
    return success;
}


// 生成矩形的顶点数据和纹理坐标
void createRectangleData(std::vector<float>& vertices, std::vector<unsigned int>& indices) {
    // 矩形的四个顶点 (x, y, z, u, v)
    // 使用标准化设备坐标，范围在-1到1之间，无需宽高参数
    vertices = {
        // 左下
        -1.0f, -1.0f, 0.0f,  0.0f, 1.0f,
        // 左上
        -1.0f,  1.0f, 0.0f,  0.0f, 0.0f,
        // 右上
        1.0f,  1.0f, 0.0f,  1.0f, 0.0f,
        // 右下
        1.0f, -1.0f, 0.0f,  1.0f, 1.0f
    };
    
    // 矩形的索引数据（两个三角形组成一个矩形）
    indices = {
        0, 1, 2,  // 第一个三角形
        0, 2, 3   // 第二个三角形
    };
    
}

void setupRectangle(unsigned int &VAO, unsigned int &VBO, unsigned int &EBO) {
    std::vector<float> vertices;
    std::vector<unsigned int> indices;
    
    // 生成矩形顶点数据
    createRectangleData(vertices, indices);
    
    glGenVertexArrays(1, &VAO);
    glGenBuffers(1, &VBO);
    glGenBuffers(1, &EBO);
    
    glBindVertexArray(VAO);
    
    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(float), vertices.data(), GL_STATIC_DRAW);
    
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, EBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(unsigned int), indices.data(), GL_STATIC_DRAW);
    
    // 配置顶点属性指针（位置和纹理坐标）
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
}

- (void)_setupOpenGL {
    [EAGLContext setCurrentContext:glView.context];
    
    // 生成和配置 VAO、VBO 和 EBO，使用矩形替代球体
    unsigned int EBO;
    setupRectangle(_VAO, _VBO, EBO);
    
    // 设置 OpenGL 状态
    glEnable(GL_DEPTH_TEST);
    
    // 对于矩形，面剔除可以优化性能
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CW);
    
    // 初始化纹理（保持不变）
    glGenTextures(1, &_yTexture);
    [self _configTexture:_yTexture];
    
    glGenTextures(1, &_uTexture);
    [self _configTexture:_uTexture];
    
    glGenTextures(1, &_vTexture);
    [self _configTexture:_vTexture];
    
    glBindVertexArray(0);
}

- (void)_configTexture:(GLuint)texture {
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glGenerateMipmap(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 0);
}

float rad = 0;
int first = 0;
//创建投影矩阵
glm::mat4  projection = glm::mat4(1.0f);//单位矩阵初始化
glm::mat4 view = glm::mat4(1.0f);

- (void)displayWithFrame:(AVFrame *)yuvFrame bb:(void (^)(BOOL success))completionBlock{
    
    if (yuvFrame == NULL) {
        if (completionBlock) completionBlock(NO);
        return;
    }
    dispatch_async(dispatch_get_main_queue(), ^{
        [EAGLContext setCurrentContext:self->glView.context];
        int videoWidth = yuvFrame->width;
        int videoHeight = yuvFrame->height;
        
        if (videoWidth <= 0 || videoHeight <= 0 || !yuvFrame) {
            if (completionBlock) completionBlock(NO);
            return;
        }
        /*
         AVFrame->width：表示视频的有效像素宽度（实际显示的宽度）。自己用就会花屏
         AVFrame->linesize[0]：表示 Y 分量在内存中的实际存储宽度（包含对齐填充的无效数据）。
         */
        // 获取各分量实际行间距
        int yStride = yuvFrame->linesize[0];
        int uStride = yuvFrame->linesize[1];
        int vStride = yuvFrame->linesize[2];
        
        // 计算UV分量尺寸
        int uvWidth = videoWidth / 2;
        int uvHeight = videoHeight / 2;
        
        // 关键：计算宽向校正因子（有效宽度 / 实际行间距）
        float yScaleX = (float)videoWidth / yStride;    // Y分量水平校正
        float uvScaleX = (float)uvWidth / uStride;      // U/V分量水平校正
        (void)uvScaleX;
        
        glBindFramebuffer(GL_FRAMEBUFFER, self->_frameBuffer);
        glBindRenderbuffer(GL_RENDERBUFFER, self->_colorRenderBuffer);
        
        GLint fboW = 0, fboH = 0;
        glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, &fboW);
        glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT, &fboH);
        if (fboW <= 0 || fboH <= 0) {
            if (completionBlock) completionBlock(NO);
            return;
        }
        
        // 用 fence 等 GPU，避免 draw 测成 0.00
        double t0 = sc_gl_now_ms();
        
        glEnable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        /* 先全屏清黑，等比例时黑边来自这里 */
        glViewport(0, 0, fboW, fboH);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glUseProgram(self->_glProgram);
        {
            GLsync s = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
            glClientWaitSync(s, GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
            glDeleteSync(s);
        }
        double t_c1 = sc_gl_now_ms();
        
        // 绑定Y纹理
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, self->_yTexture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE,
                     yStride, videoHeight, 0,
                     GL_LUMINANCE, GL_UNSIGNED_BYTE, yuvFrame->data[0]);
        glUniform1i(glGetUniformLocation(self->_glProgram, "yTexture"), 0);
        
        // 绑定U纹理
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, self->_uTexture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE,
                     uStride, uvHeight, 0,
                     GL_LUMINANCE, GL_UNSIGNED_BYTE, yuvFrame->data[1]);
        glUniform1i(glGetUniformLocation(self->_glProgram, "uTexture"), 1);
        
        // 绑定V纹理
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, self->_vTexture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE,
                     vStride, uvHeight, 0,
                     GL_LUMINANCE, GL_UNSIGNED_BYTE, yuvFrame->data[2]);
        glUniform1i(glGetUniformLocation(self->_glProgram, "vTexture"), 2);
        {
            GLsync s = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
            glClientWaitSync(s, GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
            glDeleteSync(s);
        }
        double t_u1 = sc_gl_now_ms();
        
        // GL 纹理原点修正 + 流旋转元数据（竖屏常带 90/270）
        glm::mat4 projection = glm::mat4(1.0f);
        glm::mat4 view = glm::mat4(1.0f);
        glm::mat4 model = glm::mat4(1.0f);
        model = glm::rotate(model, glm::radians(180.0f), glm::vec3(0.0f, 0.0f, 1.0f));
        model = glm::scale(model, glm::vec3(-1.0f, 1.0f, 1.0f));
        {
            int rot = ((self.rotateDegrees % 360) + 360) % 360;
            if (rot != 0) {
                model = glm::rotate(model, glm::radians((float)rot), glm::vec3(0.0f, 0.0f, 1.0f));
            }
        }
        
        glUniformMatrix4fv(glGetUniformLocation(self->_glProgram, "projection"), 1, GL_FALSE, &projection[0][0]);
        glUniformMatrix4fv(glGetUniformLocation(self->_glProgram, "view"), 1, GL_FALSE, &view[0][0]);
        glUniformMatrix4fv(glGetUniformLocation(self->_glProgram, "model"), 1, GL_FALSE, &model[0][0]);
        
        float videoToRenderRatio = yScaleX;
        glUniform1f(glGetUniformLocation(self->_glProgram, "videoToRenderRatio"), videoToRenderRatio);
        
        /* 显示模式：等比例 letterbox / 拉伸铺满（90/270 交换宽高算比例） */
        if (self.fillMode == SCRenderFillModeAspectFit) {
            float viewAspect = (float)fboW / (float)fboH;
            int rot = ((self.rotateDegrees % 360) + 360) % 360;
            float dispW = (float)videoWidth;
            float dispH = (float)videoHeight;
            if (rot == 90 || rot == 270) {
                dispW = (float)videoHeight;
                dispH = (float)videoWidth;
            }
            float videoAspect = dispW / dispH;
            GLint vpX = 0, vpY = 0, vpW = fboW, vpH = fboH;
            if (videoAspect > viewAspect) {
                vpW = fboW;
                vpH = (GLint)(fboW / videoAspect + 0.5f);
                vpY = (fboH - vpH) / 2;
            } else {
                vpH = fboH;
                vpW = (GLint)(fboH * videoAspect + 0.5f);
                vpX = (fboW - vpW) / 2;
            }
            glViewport(vpX, vpY, vpW, vpH);
        } else {
            glViewport(0, 0, fboW, fboH);
        }
        
        glBindVertexArray(self->_VAO);
        double t_d0 = sc_gl_now_ms();
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
        {
            GLsync s = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
            glClientWaitSync(s, GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
            glDeleteSync(s);
        }
        double t_d1 = sc_gl_now_ms();
        
        double t_p0 = sc_gl_now_ms();
        [self resolveMSAAIfNeededAndPresent];
        {
            GLsync s = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
            glClientWaitSync(s, GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
            glDeleteSync(s);
        }
        double t_p1 = sc_gl_now_ms();
        
        double clear_ms = t_c1 - t0;
        double upload_ms = t_u1 - t_c1;
        double draw_ms = t_d1 - t_d0;
        double present_ms = t_p1 - t_p0;
        double total_ms = t_p1 - t0;
//        printf("GL: clear=%.3f ms | upload=%.3f ms | draw=%.3f ms | present=%.3f ms | total=%.3f ms\n",
//               clear_ms, upload_ms, draw_ms, present_ms, total_ms);
        if (completionBlock) completionBlock(YES);
    });
}

- (void)resolveMSAAIfNeededAndPresent {
    if (self->_useMSAA && self->_msaaFrameBuffer && self->_resolveFrameBuffer) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, self->_msaaFrameBuffer);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, self->_resolveFrameBuffer);
        glBlitFramebuffer(0, 0, self->_fboWidth, self->_fboHeight,
                          0, 0, self->_fboWidth, self->_fboHeight,
                          GL_COLOR_BUFFER_BIT, GL_LINEAR);
    }
    glBindRenderbuffer(GL_RENDERBUFFER, self->_colorRenderBuffer);
    [glView.context presentRenderbuffer:GL_RENDERBUFFER];
}

- (void)clearDisplay {
    dispatch_async(dispatch_get_main_queue(), ^{
        if (!self->glView.context || !self->_frameBuffer) {
            return;
        }
        [EAGLContext setCurrentContext:self->glView.context];
        glBindFramebuffer(GL_FRAMEBUFFER, self->_frameBuffer);
        if (self->_fboWidth > 0 && self->_fboHeight > 0) {
            glViewport(0, 0, self->_fboWidth, self->_fboHeight);
        }
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        [self resolveMSAAIfNeededAndPresent];
    });
}






- (void)setupFrameAndRenderBuffer
{
    [EAGLContext setCurrentContext:glView.context];

    // drawable color buffer
    glGenRenderbuffers(1, &_colorRenderBuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, _colorRenderBuffer);
    if (![glView.context renderbufferStorage:GL_RENDERBUFFER fromDrawable:_eaglLayer]) {
        NSLog(@"Error: renderbufferStorage fromDrawable failed");
        return;
    }

    GLint width = 0, height = 0;
    glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, &width);
    glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT, &height);
    if (width <= 0 || height <= 0) {
        NSLog(@"Error: renderbuffer size is %dx%d (layer bounds=%@ scale=%.1f)",
              width, height, NSStringFromCGRect(_eaglLayer.bounds), _eaglLayer.contentsScale);
        return;
    }
    _fboWidth = width;
    _fboHeight = height;
    glViewport(0, 0, width, height);

    glGenFramebuffers(1, &_resolveFrameBuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, _resolveFrameBuffer);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                              GL_RENDERBUFFER, _colorRenderBuffer);

    _msaaSamples = [self _samplesForMSAALevel];
    _useMSAA = (_msaaSamples > 0);
    if (_useMSAA) {
        glGenFramebuffers(1, &_msaaFrameBuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, _msaaFrameBuffer);

        glGenRenderbuffers(1, &_msaaColorBuffer);
        glBindRenderbuffer(GL_RENDERBUFFER, _msaaColorBuffer);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, _msaaSamples, GL_RGBA8, width, height);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                  GL_RENDERBUFFER, _msaaColorBuffer);

        glGenRenderbuffers(1, &_msaaDepthBuffer);
        glBindRenderbuffer(GL_RENDERBUFFER, _msaaDepthBuffer);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, _msaaSamples, GL_DEPTH_COMPONENT16, width, height);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                  GL_RENDERBUFFER, _msaaDepthBuffer);

        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            NSLog(@"MSAA FBO incomplete status=0x%x samples=%d, fallback no-AA", status, _msaaSamples);
            _useMSAA = NO;
            _msaaSamples = 0;
            if (_msaaColorBuffer) { glDeleteRenderbuffers(1, &_msaaColorBuffer); _msaaColorBuffer = 0; }
            if (_msaaDepthBuffer) { glDeleteRenderbuffers(1, &_msaaDepthBuffer); _msaaDepthBuffer = 0; }
            if (_msaaFrameBuffer) { glDeleteFramebuffers(1, &_msaaFrameBuffer); _msaaFrameBuffer = 0; }
        } else {
            _frameBuffer = _msaaFrameBuffer;
        }
    }

    if (!_useMSAA) {
        glGenRenderbuffers(1, &_depthRenderBuffer);
        glBindRenderbuffer(GL_RENDERBUFFER, _depthRenderBuffer);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, width, height);
        glBindFramebuffer(GL_FRAMEBUFFER, _resolveFrameBuffer);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                  GL_RENDERBUFFER, _depthRenderBuffer);
        _frameBuffer = _resolveFrameBuffer;
    }

    glBindRenderbuffer(GL_RENDERBUFFER, _colorRenderBuffer);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (_useMSAA) {
        glBindFramebuffer(GL_FRAMEBUFFER, _msaaFrameBuffer);
        status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    }
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        NSLog(@"Error: Frame buffer is not completed. status=0x%x size=%dx%d msaa=%d",
              status, width, height, _msaaSamples);
        return;
    }
    NSLog(@"SCRender FBO %dx%d scale=%.1f msaa=%dx quality=%ld",
          width, height, _eaglLayer.contentsScale, _msaaSamples, (long)self.quality);
}

@end
