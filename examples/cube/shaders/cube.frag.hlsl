// The fragment shader of examples/cube: the color of the texture, a little darker toward the
// edges of each face. SDL_GPU: the samplers of a fragment shader are in space2 (spec §8.2).

Texture2D<float4> crate : register(t0, space2);
SamplerState smooth : register(s0, space2);

float4 main(float2 uv : TEXCOORD0) : SV_Target0
{
    float2 centered = uv - 0.5;
    float shade = 1.0 - 0.6 * dot(centered, centered);
    float4 color = crate.Sample(smooth, uv);
    return float4(color.rgb * shade, color.a);
}
