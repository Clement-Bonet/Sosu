// The vertex shader of tests/programs/gpu: positions already in clip space (z for the depth),
// texture coordinates and a color passed on.

struct Input
{
    float3 position : TEXCOORD0;  // location 0
    float2 uv : TEXCOORD1;        // location 1
    float4 color : TEXCOORD2;     // location 2: four bytes read as 0..1
};

struct Output
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : TEXCOORD1;
};

Output main(Input input)
{
    Output output;
    output.position = float4(input.position, 1.0);
    output.uv = input.uv;
    output.color = input.color;
    return output;
}
