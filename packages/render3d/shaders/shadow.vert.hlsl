// The vertex shader of the shadows of render3d: a vertex seen by the sun, its depth alone.

cbuffer Object : register(b0, space1)
{
    float4x4 model;
    float4x4 light_view_projection;
};

float4 main(float3 position : TEXCOORD0) : SV_Position
{
    return mul(light_view_projection, mul(model, float4(position, 1.0)));
}
