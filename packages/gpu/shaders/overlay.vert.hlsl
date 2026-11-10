// The vertex shader of gpu.overlay: one triangle that covers the target, from the index of its
// vertex alone (no vertex buffer); texture coordinates 0, 0 at the top left.

struct Output
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

Output main(uint id : SV_VertexID)
{
    Output output;
    float2 uv = float2((id << 1) & 2, id & 2);
    output.uv = uv;
    output.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}
