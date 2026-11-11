// The fragment shader of particles: the texture (or a frame of its flipbook) times the color of
// the particle.

Texture2D<float4> sprite : register(t0, space2);
SamplerState sprite_sampler : register(s0, space2);

struct Input
{
    float2 uv : TEXCOORD0;
    float4 color : TEXCOORD1;
};

float4 main(Input input) : SV_Target
{
    return sprite.Sample(sprite_sampler, input.uv) * input.color;
}
