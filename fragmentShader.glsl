precision mediump float;

uniform sampler2D myTexture;
uniform sampler2D samplerU;
uniform sampler2D samplerV;

varying vec2 myTextureCoordsOut;

//void main()
//{
//    // 用一个矩阵来简化后面YUV转GRB的计算公式
//    mat3 conversionColor = mat3(1.164, 1.164, 1.164,
//                                0.0, -0.213, 2.112,
//                                1.793, -0.533, 0.0);
//
//    mediump vec3 yuv;
//    lowp vec3 rgb;
//
//    yuv.x = texture2D(myTexture, myTextureCoordsOut).r - (16.0/255.0);
//    yuv.yz = texture2D(samplerUV, myTextureCoordsOut).rg - vec2(0.5, 0.5);
//
//    rgb = conversionColor * yuv;
//
//
//    gl_FragColor = vec4(rgb, 1.0);
////    gl_FragColor = vec4(texture2D(myTexture, myTextureCoordsOut).rgb, 1.0);
//}


void main()
{
    
    highp float y = texture(myTexture, myTextureCoordsOut).r;
    highp float cb = texture(samplerU, myTextureCoordsOut).r;
    highp float cr = texture(samplerV, myTextureCoordsOut).r;
    
    
    /// 按YCbCr转RGB的公式进行数据转换
    highp float r = y + 1.403 * (cr - 0.5);
    highp float g = y - 0.343 * (cb - 0.5) - 0.714 * (cr - 0.5);
    highp float b = y + 1.770 * (cb - 0.5);
    // 通过纹理坐标数据来获取对应坐标色值并传递
    gl_FragColor = vec4(r, g, b, 1.0);
    
}



