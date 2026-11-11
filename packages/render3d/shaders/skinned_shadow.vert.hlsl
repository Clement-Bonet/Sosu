// The vertex shader of the shadows of skinned meshes: a vertex moved by its joints, seen by the sun.

cbuffer Object : register(b0, space1)
{
    float4x4 model;
    float4x4 light_view_projection;
};

cbuffer Skin : register(b1, space1)
{
    float4x4 joints[128];
};

struct Input
{
    float3 position : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float4 joint : TEXCOORD3;
    float4 weight : TEXCOORD4;
};

float4 main(Input input) : SV_Position
{
    float4x4 skin = joints[(uint)input.joint.x] * input.weight.x + joints[(uint)input.joint.y] * input.weight.y
        + joints[(uint)input.joint.z] * input.weight.z + joints[(uint)input.joint.w] * input.weight.w;
    return mul(light_view_projection, mul(model, mul(skin, float4(input.position, 1.0))));
}
