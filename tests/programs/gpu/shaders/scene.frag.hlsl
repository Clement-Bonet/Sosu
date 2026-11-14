// The fragment shader of tests/programs/gpu: the color of the vertex times the texture (a white
// texture of one pixel for the shapes without one).

Texture2D<float4> picture : register(t0, space2);
SamplerState pick : register(s0, space2);

float4 main(float2 uv : TEXCOORD0, float4 color : TEXCOORD1) : SV_Target0
{
    return color * picture.Sample(pick, uv);
}
