// The vertex shader of the shadows of skinned meshes: a vertex moved by its morph targets, then by
// its joints, seen by the sun.

cbuffer Object : register(b0, space1)
{
    float4x4 model;
    float4x4 light_view_projection;
};

cbuffer Skin : register(b1, space1)
{
    float4x4 joints[128];
};

// The morph targets of the mesh, as lit.vert reads them (the normals are not read).
StructuredBuffer<float4> morphs : register(t0, space0);

cbuffer Morph : register(b2, space1)
{
    float4 morph_weights[2];
    uint4 morph_targets[2];
    uint4 morph_info;
};

struct Input
{
    float3 position : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float4 joint : TEXCOORD3;
    float4 weight : TEXCOORD4;
};

float4 main(Input input, uint vertex : SV_VertexID) : SV_Position
{
    float3 p = input.position;
    uint vertices = morph_info.y;
    for (uint k = 0; k < morph_info.x && k < 8; k++)
    {
        p += morphs[morph_targets[k >> 2][k & 3] * 2 * vertices + vertex].xyz * morph_weights[k >> 2][k & 3];
    }
    float4x4 skin = joints[(uint)input.joint.x] * input.weight.x + joints[(uint)input.joint.y] * input.weight.y
        + joints[(uint)input.joint.z] * input.weight.z + joints[(uint)input.joint.w] * input.weight.w;
    return mul(light_view_projection, mul(model, mul(skin, float4(p, 1.0))));
}
