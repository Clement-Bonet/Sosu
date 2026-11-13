// The vertex shader of render3d: a vertex of model.Vertex moved by its morph targets, placed in
// the world, seen by the camera, and by the sun for its shadows. The normals follow the model
// matrix (a scale that is the same on its three axes keeps them right).
// SDL_GPU: the storage buffers of a vertex shader in space0, its uniform buffers in space1.

cbuffer Object : register(b0, space1)
{
    float4x4 model;
    float4x4 view_projection;
    float4x4 light_view_projection;
};

// The morph targets of the mesh: for each, what it moves the positions by (one per vertex), then
// the normals; up to 8 of them given with their weights (count 0 for a mesh without).
StructuredBuffer<float4> morphs : register(t0, space0);

cbuffer Morph : register(b1, space1)
{
    float4 morph_weights[2];
    uint4 morph_targets[2];
    uint4 morph_info;  // x: the targets given; y: the vertices of the mesh
};

struct Input
{
    float3 position : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
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
    Output output;
    float4 world = mul(model, float4(position, 1.0));
    output.position = mul(view_projection, world);
    output.world = world.xyz;
    output.normal = mul((float3x3)model, normal);
    output.uv = input.uv;
    output.light = mul(light_view_projection, world);
    return output;
}
