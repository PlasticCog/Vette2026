// The Enhanced 3D view with a depth buffer (Presenter's GPU pass), for Direct3D 12 (DXIL).
// Vertices are enhanced::SceneVertex: race-frame position, colour (straight alpha), reversed depth, and a
// second colour with a dither flag. The vertex shader maps the frame to clip space with `xf`
// (x * xf.x + xf.z, y * xf.y + xf.w) and halves the depth, which may run a little past 1 with its
// draw-order bias (see scene.h). The pixel shader draws a dithered face as the original's checkerboard:
// the second colour on the odd cells of the race frame's pixel grid (`cell`: a cell's size in target
// pixels, and whether dithering is on), the first on the even.
// Rebuild the compiled forms with build_shaders.py (beside this file) after editing.

cbuffer Transform : register(b0, space1) {
    float4 xf;
};

cbuffer Dither : register(b0, space3) {
    float4 cell;  // x, y: a race pixel in target pixels; z: 1 with dithering on
};

struct VSInput {
    float2 position : TEXCOORD0;
    float4 color : TEXCOORD1;
    float depth : TEXCOORD2;
    float4 color2 : TEXCOORD3;
    float dither : TEXCOORD4;
};

struct VSOutput {
    float4 color : TEXCOORD0;
    float4 color2 : TEXCOORD1;  // premultiplied; alpha: the dither flag
    float4 position : SV_Position;
};

VSOutput vs_main(VSInput input) {
    VSOutput output;
    output.position = float4(input.position.x * xf.x + xf.z, input.position.y * xf.y + xf.w, input.depth * 0.5, 1.0);
    output.color = float4(input.color.rgb * input.color.a, input.color.a);  // premultiplied
    output.color2 = float4(input.color2.rgb * input.color.a, input.dither);
    return output;
}

float4 ps_main(VSOutput input) : SV_Target0 {
    if (input.color2.a > 0.5 && cell.z > 0.5) {
        const float2 c = floor(input.position.xy / cell.xy);
        if (fmod(c.x + c.y, 2.0) >= 1.0)
            return float4(input.color2.rgb, input.color.a);
    }
    return input.color;
}
