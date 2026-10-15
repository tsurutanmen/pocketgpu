#version 450
// GZDoom の Vulkan の使い方をまねた試験（gzlike.c）の画素シェーダー。
// 結果は整数だけで決まるので、どの GPU でも同じ値になる（gzlike.c が CPU で同じ計算をして比べる）。
layout(set = 0, binding = 0) uniform sampler2D lightmap;
layout(set = 0, binding = 1) uniform sampler2D shadowmap;
layout(set = 1, binding = 0) uniform Viewpoint { ivec4 v0; ivec4 v1; ivec4 pad[14]; } vp;
layout(set = 1, binding = 2) uniform Stream { ivec4 tint; ivec4 info; ivec4 pad[14]; } st;
layout(set = 1, binding = 3) buffer Lights { uint counter; uint pad0; uint pad1; uint pad2; ivec4 lights[256]; } lb;
layout(set = 2, binding = 0) uniform sampler2D tex0;
layout(set = 2, binding = 1) uniform sampler2D tex1;
layout(push_constant) uniform PC { int boneIdx; int lightIdx; int pad0; int pad1; ivec4 f[7]; } pc;
layout(location = 0) in vec2 uv;
layout(location = 1) flat in int boneAdd;
layout(location = 0) out vec4 o0;
layout(location = 1) out vec4 o1;
void main() {
    ivec3 c0 = ivec3(round(texture(tex0, uv).rgb * 255.0));
    ivec2 tp = ivec2(uv * 16.0);
    ivec3 c1 = ivec3(round(texelFetch(tex1, tp, 0).rgb * 255.0));
    ivec2 sp = ivec2(gl_FragCoord.xy) & 15;
    int lm = int(round(texelFetch(lightmap, sp, 0).r * 255.0)) & 3;
    int sm = int(round(texelFetch(shadowmap, sp, 0).g * 255.0)) & 3;
    ivec3 base = (3 * c0 + c1) >> 2;
    base += st.tint.rgb;
    base += lb.lights[pc.lightIdx & 255].rgb;
    base.r += boneAdd;
    base.b += pc.f[3].y;
    base.g += lm;
    base.r += sm;
    int mode = st.info.x;
    int fb = vp.v0.z;
    if (mode == 1) base ^= ivec3(fb);
    else if (mode == 2) base += ivec3(fb);
    else if (mode == 3) base = base.bgr;
    base = min(base, ivec3(255));
    atomicAdd(lb.counter, 1u);
    o0 = vec4(vec3(base) / 255.0, 1.0);
    o1 = vec4(vec3(c1) / 255.0, 1.0);
}
