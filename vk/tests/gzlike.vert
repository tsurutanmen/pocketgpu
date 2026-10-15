#version 450
// GZDoom の Vulkan の使い方をまねた試験（gzlike.c）の頂点シェーダー。
// set 1 は dynamic offset 付きの UBO 3 個（0 viewpoint・1 matrices・2 stream）と storage buffer 2 個（3 lights・4 bones）。
layout(set = 1, binding = 0) uniform Viewpoint { ivec4 v0; ivec4 v1; ivec4 pad[14]; } vp;
layout(set = 1, binding = 1) uniform Matrices { mat4 model; mat4 normalModel; mat4 texMat; } mt;
layout(set = 1, binding = 4) readonly buffer Bones { ivec4 bones[256]; } bn;
layout(push_constant) uniform PC { int boneIdx; int lightIdx; int pad0; int pad1; ivec4 f[7]; } pc;
layout(location = 0) in vec2 corner;
layout(location = 0) out vec2 uv;
layout(location = 1) flat out int boneAdd;
void main() {
    vec4 p = mt.model * vec4(corner, 0.0, 1.0);
    ivec4 b = bn.bones[pc.boneIdx & 255];
    p.xy += vec2(b.xy);
    gl_Position = vec4(p.x * 2.0 / float(vp.v0.x) - 1.0, p.y * 2.0 / float(vp.v0.y) - 1.0, 0.0, 1.0);
    uv = (mt.texMat * vec4(corner, 0.0, 1.0)).xy;
    boneAdd = b.w;
}
