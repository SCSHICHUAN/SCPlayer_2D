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

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#import "Camera.h"
//#import "Public.h"




#import "JpegUtil.h"

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
    
    GLuint          _colorRenderBuffer;
    GLuint          _depthRenderBuffer;
    GLuint          _frameBuffer;
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
        [self config];
    }
    return self;
}

-(void)config{
    [self _creatOpenGLContent];
    [self _setupOpenGLProgram];
    [self _setupOpenGL];
}

-(void)_creatOpenGLContent{
    glView = [[GLKView alloc] initWithFrame:self.bounds];
    glView.context = [[EAGLContext alloc] initWithAPI:kEAGLRenderingAPIOpenGLES3];
    [EAGLContext setCurrentContext:glView.context];
    [self addSubview:glView];
    _eaglLayer = glView.layer;
    [self setupFrameAndRenderBuffer];
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
    
    dispatch_async(dispatch_get_main_queue(), ^{
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
        
        
        // 初始化GL状态
        glEnable(GL_DEPTH_TEST);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glUseProgram(self->_glProgram);
        
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
        
       
        
        // 保持旋转矩阵
        glm::mat4 projection = glm::mat4(1.0f);
        glm::mat4 view = glm::mat4(1.0f);
        glm::mat4 model = glm::mat4(1.0f);
        model = glm::rotate(model, glm::radians(180.0f), glm::vec3(0.0f, 0.0f, 1.0f));
        
        glUniformMatrix4fv(glGetUniformLocation(self->_glProgram, "projection"), 1, GL_FALSE, &projection[0][0]);
        glUniformMatrix4fv(glGetUniformLocation(self->_glProgram, "view"), 1, GL_FALSE, &view[0][0]);
        glUniformMatrix4fv(glGetUniformLocation(self->_glProgram, "model"), 1, GL_FALSE, &model[0][0]);
        
        
        //YUV的宽度没有渲染的宽度一样时拉伸x坐标
        float videoToRenderRatio = yScaleX;
        glUniform1f(glGetUniformLocation(self->_glProgram, "videoToRenderRatio"), videoToRenderRatio);
       
        
        // 绘制
        glBindVertexArray(self->_VAO);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
        
        // 呈现结果
        [self->glView.context presentRenderbuffer:GL_RENDERBUFFER];
        if (completionBlock) completionBlock(YES);
        
        
    });
}






- (void)setupFrameAndRenderBuffer
{
    // Setup color render buffer
    glGenRenderbuffers(1, &_colorRenderBuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, _colorRenderBuffer);
    [glView.context renderbufferStorage:GL_RENDERBUFFER fromDrawable:_eaglLayer];
    
    // Setup depth render buffer
    int width, height;
    glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, &width);
    glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT, &height);
    
    glViewport(0, 0, width, height);
    
    // Create a depth buffer that has the same size as the color buffer.
    glGenRenderbuffers(1, &_depthRenderBuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, _depthRenderBuffer);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, width, height);
    
    // Setup frame buffer
    glGenFramebuffers(1, &_frameBuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, _frameBuffer);
    
    // Attach color render buffer and depth render buffer to frameBuffer
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                              GL_RENDERBUFFER, _colorRenderBuffer);
    
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, _depthRenderBuffer);
    
    // Set color render buffer as current render buffer
    glBindRenderbuffer(GL_RENDERBUFFER, _colorRenderBuffer);
    
    // Check FBO satus
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        NSLog(@"Error: Frame buffer is not completed.");
        exit(1);
    }
}

@end
