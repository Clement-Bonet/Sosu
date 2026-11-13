// The vertex shader of render3d for skinned meshes: a vertex moved by its morph targets, then by up
// to four joints (the matrices of `anim.palette`, by their weights, as glTF has it), then placed
// and lit as lit.vert does. The palette is a uniform buffer of 128 matrices: a skeleton of more
// joints is drawn at rest.

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

// The morph targets of the mesh, as lit.vert reads them.
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

struct Output
{
    float4 position : SV_Position;
    float3 world : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float4 light : TEXCOORD3;
};

Output main(Input input, uint vertex : SV_VertexID)
{
    float3 position = input.position;
    float3 normal = input.normal;
    uint vertices = morph_info.y;
    for (uint k = 0; k < morph_info.x && k < 8; k++)
    {
        uint t = morph_targets[k >> 2][k & 3];
        float w = morph_weights[k >> 2][k & 3];
        position += morphs[t * 2 * vertices + vertex].xyz * w;
        normal += morphs[t * 2 * vertices + vertices + vertex].xyz * w;
    }
    float4x4 skin = joints[(uint)input.joint.x] * input.weight.x + joints[(uint)input.joint.y] * input.weight.y
        + joints[(uint)input.joint.z] * input.weight.z + joints[(uint)input.joint.w] * input.weight.w;
    Output output;
    float4 world = mul(model, mul(skin, float4(position, 1.0)));
    output.position = mul(view_projection, world);
    output.world = world.xyz;
    output.normal = mul((float3x3)model, mul((float3x3)skin, normal));
    output.uv = input.uv;
    output.light = mul(light_view_projection, world);
    return output;
}
