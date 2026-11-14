// The fragment shader of the lines of render3d: their color, blended over the scene.

float4 main(float4 position : SV_Position, float4 color : TEXCOORD0) : SV_Target
{
    return color;
}
