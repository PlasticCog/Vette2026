// The Enhanced 3D view with a depth buffer (Presenter's GPU pass), for Metal (MSL, compiled by SDL at
// run time): see scene.hlsl.
#include <metal_stdlib>
using namespace metal;

struct VSInput {
    float2 position [[attribute(0)]];
    float4 color [[attribute(1)]];
    float depth [[attribute(2)]];
    float4 color2 [[attribute(3)]];
    float dither [[attribute(4)]];
};

struct VSOutput {
    float4 position [[position]];
    float4 color;
    float4 color2;  // premultiplied; alpha: the dither flag
};

struct Transform {
    float4 xf;
};

struct Dither {
    float4 cell;  // x, y: a race pixel in target pixels; z: 1 with dithering on
};

vertex VSOutput vs_main(VSInput input [[stage_in]], constant Transform& t [[buffer(0)]]) {
    VSOutput output;
    output.position = float4(input.position.x * t.xf.x + t.xf.z, input.position.y * t.xf.y + t.xf.w,
                             input.depth * 0.5, 1.0);
    output.color = float4(input.color.rgb * input.color.a, input.color.a);  // premultiplied
    output.color2 = float4(input.color2.rgb * input.color.a, input.dither);
    return output;
}

fragment float4 fs_main(VSOutput input [[stage_in]], constant Dither& d [[buffer(0)]]) {
    if (input.color2.a > 0.5 && d.cell.z > 0.5) {
        const float2 c = floor(input.position.xy / d.cell.xy);
        if (fmod(c.x + c.y, 2.0) >= 1.0)
            return float4(input.color2.rgb, input.color.a);
    }
    return input.color;
}
