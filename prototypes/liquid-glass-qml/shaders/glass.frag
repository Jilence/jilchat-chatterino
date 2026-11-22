#version 440
// Liquid Glass for Qt Quick: port of jiltv-frontend/src/lib/glass/liquid-glass.ts.
//
// The rim is a convex squircle lens (IOR 1.5). Snell's law gives how far the
// background under each point of the rim is shifted inward. The website bakes
// that into an SVG displacement map; here it is computed per pixel on the GPU,
// so there is nothing to rebuild when the glass changes size.

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec4 srcRect;     // this quad inside the source texture (uv offset, uv size)
    vec2 size;        // quad size in px (glass + shadow padding)
    vec4 box;         // glass rect inside the quad, px
    float radius;
    float bezel;      // width of the curved rim, px
    float thickness;  // glass height, px; thicker glass bends more
    float frost;      // blur as mip level
    float dispersion; // extra bend for blue vs red: rainbow fringe at the rim
    float specular;
    float saturation;
    float brightness;
    vec4 tint;        // rgb + amount
    float shadow;
    float press;      // 0..1 interactive highlight
    float zoom;       // >1 magnifies the flat top (lens)
    float flipY;
};
layout(binding = 1) uniform sampler2D src;

const float IOR = 1.5;

const vec2 DISK[12] = vec2[](
    vec2(-0.326, -0.406), vec2(-0.840, -0.074), vec2(-0.696, 0.457), vec2(-0.203, 0.621),
    vec2(0.962, -0.195), vec2(0.473, -0.480), vec2(0.519, 0.767), vec2(0.185, -0.893),
    vec2(0.507, 0.064), vec2(0.896, 0.412), vec2(-0.322, -0.933), vec2(-0.792, -0.598));

float sdRoundBox(vec2 p, vec2 b, float r)
{
    vec2 q = abs(p) - b + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

// Outward normal of the rounded rect.
vec2 sdNormal(vec2 p, vec2 b, float r)
{
    vec2 s = vec2(p.x < 0.0 ? -1.0 : 1.0, p.y < 0.0 ? -1.0 : 1.0);
    vec2 q = abs(p) - (b - r);
    if (q.x > 0.0 && q.y > 0.0)
        return normalize(q) * s;
    return q.x > q.y ? vec2(s.x, 0.0) : vec2(0.0, s.y);
}

// Convex squircle height profile: flat top, smooth roll-off at the edge.
float surf(float x) { return pow(1.0 - pow(1.0 - x, 4.0), 0.25); }
float dsurf(float x)
{
    float a = 1.0 - x;
    return a * a * a * pow(max(1.0 - a * a * a * a, 1e-6), -0.75);
}

// Inward shift of the background at rim position t (0 = outer edge, 1 = flat top).
float shiftAt(float t, float bz)
{
    float x = clamp(t, 0.002, 0.998);
    float incidence = atan(dsurf(x) * thickness / bz);
    float refracted = asin(sin(incidence) / IOR);
    return surf(x) * thickness * tan(incidence - refracted);
}

vec2 fixUv(vec2 uv)
{
    uv = clamp(uv, vec2(0.0), vec2(1.0));
    return flipY > 0.5 ? vec2(uv.x, 1.0 - uv.y) : uv;
}

vec3 sampleSrc(vec2 uv)
{
    if (frost < 0.05)
        return texture(src, fixUv(uv)).rgb;
    vec2 texel = 1.0 / vec2(textureSize(src, 0));
    vec2 rad = texel * exp2(frost) * 1.25;
    vec3 acc = textureLod(src, fixUv(uv), frost).rgb;
    for (int i = 0; i < 12; ++i)
        acc += textureLod(src, fixUv(uv + DISK[i] * rad), frost).rgb;
    return acc / 13.0;
}

void main()
{
    vec2 px = vUv * size;
    vec2 hb = box.zw * 0.5;
    vec2 c = box.xy + hb;
    vec2 p = px - c;
    float r = min(radius, min(hb.x, hb.y));
    float d = sdRoundBox(p, hb, r);
    float alpha = clamp(0.5 - d, 0.0, 1.0);

    // Soft drop shadow plus a tight contact shadow, outside the glass only.
    float ds = sdRoundBox(p - vec2(0.0, 6.0), hb, r);
    float shadowA = shadow * 0.45 * (1.0 - smoothstep(-8.0, 22.0, ds))
                  + shadow * 0.22 * (1.0 - smoothstep(-1.0, 4.0, d));
    vec4 outC = vec4(0.0, 0.0, 0.0, clamp(shadowA, 0.0, 1.0));

    if (alpha > 0.0) {
        float inside = max(-d, 0.0);
        vec2 n = sdNormal(p, hb, r);
        float bz = clamp(bezel, 1.0, max(min(r, min(hb.x, hb.y)), 1.0));
        float shift = inside < bz ? shiftAt(inside / bz, bz) : 0.0;

        vec2 pxToUv = srcRect.zw / size;
        vec2 uvAt = srcRect.xy + (c + p / zoom) * pxToUv;
        vec2 off = -n * shift * pxToUv;

        vec3 col;
        if (dispersion > 0.001) {
            col.r = sampleSrc(uvAt + off * (1.0 - dispersion)).r;
            col.g = sampleSrc(uvAt + off).g;
            col.b = sampleSrc(uvAt + off * (1.0 + dispersion)).b;
        } else {
            col = sampleSrc(uvAt + off);
        }

        float l = dot(col, vec3(0.2126, 0.7152, 0.0722));
        col = mix(vec3(l), col, saturation);
        col += brightness + press * 0.06;
        col = mix(col, tint.rgb, tint.a);

        // Specular rim, light from the top left; the far rim catches a fainter reflection.
        vec2 L = normalize(vec2(-0.6, -0.8));
        float facing = dot(n, L);
        float lit = facing > 0.0 ? facing : -facing * 0.45;
        float rim = exp(-inside / 1.1) + 0.18 * exp(-inside / (bz * 0.45));
        float a = min(1.0, rim * (0.25 + 0.75 * lit * lit)) * (specular + press * 0.25);
        col = 1.0 - (1.0 - clamp(col, 0.0, 1.0)) * (1.0 - a);

        outC = vec4(col, 1.0) * alpha + outC * (1.0 - alpha);
    }

    fragColor = outC * qt_Opacity;
}
