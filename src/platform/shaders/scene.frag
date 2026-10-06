#version 450
// The Enhanced 3D view with a depth buffer (Presenter's GPU pass), for Vulkan (SPIR-V): see scene.hlsl.

layout(location = 0) in vec4 in_color;
layout(location = 0) out vec4 out_color;

void main() {
    out_color = in_color;
}
