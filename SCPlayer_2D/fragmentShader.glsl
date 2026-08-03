
precision mediump float;

uniform sampler2D planY; //位置1
uniform sampler2D planU; //位置2
uniform sampler2D planV; //位置3
varying vec2 myTextureCoordsOut;

void main()
{
    // 从各自的纹理中获取 Y、Cb 和 Cr 值
    float y = texture2D(planY, myTextureCoordsOut).r;
    float cb = texture2D(planU, myTextureCoordsOut).r;
    float cr = texture2D(planV, myTextureCoordsOut).r;

    // 按 YCbCr 转 RGB 的公式进行数据转换
    float r = y + 1.403 * (cr - 0.5);
    float g = y - 0.344 * (cb - 0.5) - 0.714 * (cr - 0.5);
    float b = y + 1.770 * (cb - 0.5);

    // 颜色反转
    

    // 通过纹理坐标数据来获取对应坐标色值并传递
    gl_FragColor = vec4(r, g, b, 1.0);
}
