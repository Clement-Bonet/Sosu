// The vertex shader of the lines of render3d: the grid of an editor, the shapes of colliders, the
// lines a game draws to debug. Seen by the camera, a little nearer to it than the surface they lie
// on, so that a line on the ground does not flicker in and out of it.

cbuffer Frame : register(b0, space1)
{
    float4x4 view_projection;
};

struct Out
{
    float4 position : SV_Position;
    float4 color : TEXCOORD0;
};

Out main(float3 position : TEXCOORD0, float4 color : TEXCOORD1)
{
    Out o;
    o.position = mul(view_projection, float4(position, 1.0));
    o.position.z -= 0.0005 * o.position.w;
    o.color = color;
    return o;
}
