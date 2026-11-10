// The vertex shader of render3d: a vertex of model.Vertex placed in the world, seen by the
// camera, and by the sun for its shadows. The normals follow the model matrix (a scale that is
// the same on its three axes keeps them right).

cbuffer Object : register(b0, space1)
{
    float4x4 model;
    float4x4 view_projection;
    float4x4 light_view_projection;
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

Output main(Input input)
{
    Output output;
    float4 world = mul(model, float4(input.position, 1.0));
    output.position = mul(view_projection, world);
    output.world = world.xyz;
    output.normal = mul((float3x3)model, input.normal);
    output.uv = input.uv;
    output.light = mul(light_view_projection, world);
    return output;
}
