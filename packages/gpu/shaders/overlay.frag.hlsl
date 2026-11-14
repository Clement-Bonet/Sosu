// The fragment shader of gpu.overlay: the pixel of the canvas, as it is (its alpha blends it over
// the 3D, in the pipeline).

Texture2D<float4> canvas : register(t0, space2);
SamplerState exact : register(s0, space2);

float4 main(float2 uv : TEXCOORD0) : SV_Target0
{
    return canvas.Sample(exact, uv);
}
