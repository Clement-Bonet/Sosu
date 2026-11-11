// The vertex shader of render3d for skinned meshes: a vertex moved by up to four joints (the
// matrices of `anim.palette`, by their weights, as glTF has it), then placed and lit as lit.vert
// does. The palette is a uniform buffer of 128 matrices: a skeleton of more joints is drawn at rest.

cbuffer Object : register(b0, space1)
{
    float4x4 model;
    float4x4 view_projection;
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

struct Output
{
    float4 position : SV_Position;
    float3 world : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float4 light : TEXCOORD3;
};

Output main(Input input)
{
    float4x4 skin = joints[(uint)input.joint.x] * input.weight.x + joints[(uint)input.joint.y] * input.weight.y
        + joints[(uint)input.joint.z] * input.weight.z + joints[(uint)input.joint.w] * input.weight.w;
    Output output;
    float4 world = mul(model, mul(skin, float4(input.position, 1.0)));
    output.position = mul(view_projection, world);
    output.world = world.xyz;
    output.normal = mul((float3x3)model, mul((float3x3)skin, input.normal));
    output.uv = input.uv;
    output.light = mul(light_view_projection, world);
    return output;
}
