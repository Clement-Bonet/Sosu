// The vertex shader of the shadows of render3d: a vertex moved by its morph targets, seen by the
// sun, its depth alone.

cbuffer Object : register(b0, space1)
{
    float4x4 model;
    float4x4 light_view_projection;
};

// The morph targets of the mesh, as lit.vert reads them (the normals are not read).
StructuredBuffer<float4> morphs : register(t0, space0);

cbuffer Morph : register(b1, space1)
{
    float4 morph_weights[2];
    uint4 morph_targets[2];
    uint4 morph_info;
};

float4 main(float3 position : TEXCOORD0, uint vertex : SV_VertexID) : SV_Position
{
    float3 p = position;
    uint vertices = morph_info.y;
    for (uint k = 0; k < morph_info.x && k < 8; k++)
    {
        p += morphs[morph_targets[k >> 2][k & 3] * 2 * vertices + vertex].xyz * morph_weights[k >> 2][k & 3];
    }
    return mul(light_view_projection, mul(model, float4(p, 1.0)));
}
