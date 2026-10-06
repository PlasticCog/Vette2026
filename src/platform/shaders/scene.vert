#version 450
// The Enhanced 3D view with a depth buffer (Presenter's GPU pass), for Vulkan (SPIR-V): see scene.hlsl.

layout(location = 0) in vec2 position;
layout(location = 1) in vec4 color;
layout(location = 2) in float depth;

layout(location = 0) out vec4 out_color;

layout(set = 1, binding = 0) uniform Transform {
    vec4 xf;
};

void main() {
    gl_Position = vec4(position.x * xf.x + xf.z, position.y * xf.y + xf.w, depth * 0.5, 1.0);
    out_color = vec4(color.rgb * color.a, color.a);  // premultiplied
}
