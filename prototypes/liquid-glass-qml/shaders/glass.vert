#version 440

layout(location = 0) in vec4 qt_Vertex;
layout(location = 1) in vec2 qt_MultiTexCoord0;
layout(location = 0) out vec2 vUv;

// Must match glass.frag exactly.
layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec4 srcRect;
    vec2 size;
    vec4 box;
    float radius;
    float bezel;
    float thickness;
    float frost;
    float dispersion;
    float specular;
    float saturation;
    float brightness;
    vec4 tint;
    float shadow;
    float press;
    float zoom;
    float flipY;
};

void main()
{
    vUv = qt_MultiTexCoord0;
    gl_Position = qt_Matrix * qt_Vertex;
}
