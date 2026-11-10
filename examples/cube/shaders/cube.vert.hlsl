// The vertex shader of examples/cube: each corner placed by the matrix of the camera and of the
// cube (math.linear: by columns, column vectors), its texture coordinates passed on.
// SDL_GPU: the uniform buffers of a vertex shader are in space1 (spec §8.2).

cbuffer Camera : register(b0, space1)
{
    float4x4 transform;  // projection * view * model
};

struct Input
{
    float3 position : TEXCOORD0;  // location 0
    float2 uv : TEXCOORD1;        // location 1
};

struct Output
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

Output main(Input input)
{
    Output output;
    output.position = mul(transform, float4(input.position, 1.0));
    output.uv = input.uv;
    return output;
}
