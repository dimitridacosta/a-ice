#version 440

layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;

layout(binding = 1) uniform sampler2D source;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    float fadeTop;    // px
    float fadeBottom; // px
    float pxPerUnit;  // hauteur de la zone en px
} ubuf;

void main()
{
    vec4 c = texture(source, qt_TexCoord0);
    float fade = smoothstep(0.0, ubuf.fadeTop / ubuf.pxPerUnit, qt_TexCoord0.y)
               * smoothstep(0.0, ubuf.fadeBottom / ubuf.pxPerUnit, 1.0 - qt_TexCoord0.y);
    fragColor = c * ubuf.qt_Opacity * clamp(fade, 0.0, 1.0);
}