#version 300 core

layout (location = 0) in vec3 pos;
layout (location = 1) in vec2 textPos;

out vec2 outTextPos;

// 实际视频宽度与渲染目标宽度的比例（由外部传递）
uniform float videoToRenderRatio;
uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;


void main() {
    gl_Position = projection * view * model * vec4(pos, 1.0);
    // 使用实际比例校正纹理坐标
    outTextPos = vec2(textPos.x * videoToRenderRatio, 1.0 - textPos.y);
}
