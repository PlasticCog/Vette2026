#version 450
// The Enhanced 3D view with a depth buffer (Presenter's GPU pass), for Vulkan (SPIR-V): see scene.hlsl.

layout(location = 0) in vec4 in_color;
layout(location = 1) in vec4 in_color2;
layout(location = 0) out vec4 out_color;

layout(set = 3, binding = 0) uniform Dither {
    vec4 cell;  // x, y: a race pixel in target pixels; z: 1 with dithering on
};

void main() {
    if (in_color2.a > 0.5 && cell.z > 0.5) {
        vec2 c = floor(gl_FragCoord.xy / cell.xy);
        if (mod(c.x + c.y, 2.0) >= 1.0) {
            out_color = vec4(in_color2.rgb, in_color.a);
            return;
        }
    }
    out_color = in_color;
}
