// The Enhanced 3D view with a depth buffer (Presenter's GPU pass), for Metal (MSL, compiled by SDL at
// run time): see scene.hlsl.
#include <metal_stdlib>
using namespace metal;

struct VSInput {
    float2 position [[attribute(0)]];
    float4 color [[attribute(1)]];
    float depth [[attribute(2)]];
};

struct VSOutput {
    float4 position [[position]];
    float4 color;
};

struct Transform {
    float4 xf;
};

vertex VSOutput vs_main(VSInput input [[stage_in]], constant Transform& t [[buffer(0)]]) {
    VSOutput output;
    output.position = float4(input.position.x * t.xf.x + t.xf.z, input.position.y * t.xf.y + t.xf.w,
                             input.depth * 0.5, 1.0);
    output.color = float4(input.color.rgb * input.color.a, input.color.a);  // premultiplied
    return output;
}

fragment float4 fs_main(VSOutput input [[stage_in]]) {
    return input.color;
}
