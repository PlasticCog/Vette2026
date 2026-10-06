// The Enhanced 3D view with a depth buffer (Presenter's GPU pass), for Direct3D 12 (DXIL).
// Vertices are enhanced::SceneVertex: race-frame position, colour (straight alpha), reversed depth.
// The vertex shader maps the frame to clip space with `xf` (x * xf.x + xf.z, y * xf.y + xf.w) and
// halves the depth, which may run a little past 1 with its draw-order bias (see scene.h).
// Rebuild the compiled forms with build_shaders.py (beside this file) after editing.

cbuffer Transform : register(b0, space1) {
    float4 xf;
};

struct VSInput {
    float2 position : TEXCOORD0;
    float4 color : TEXCOORD1;
    float depth : TEXCOORD2;
};

struct VSOutput {
    float4 color : TEXCOORD0;
    float4 position : SV_Position;
};

VSOutput vs_main(VSInput input) {
    VSOutput output;
    output.position = float4(input.position.x * xf.x + xf.z, input.position.y * xf.y + xf.w, input.depth * 0.5, 1.0);
    output.color = float4(input.color.rgb * input.color.a, input.color.a);  // premultiplied
    return output;
}

float4 ps_main(float4 color : TEXCOORD0) : SV_Target0 {
    return color;
}
