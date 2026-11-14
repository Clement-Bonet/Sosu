// The vertex shader of particles.compute: six vertices per place of the buffer of particles, the
// corners of its billboard (facing the camera, or stretched along its velocity), from what the
// compute shader left there. A free or dead place gives nothing on the screen.
// SDL_GPU: storage buffers of a vertex shader in space0, its uniform buffers in space1.

struct Particle
{
    float4 position_age;
    float4 velocity_life;
    float4 size_turn;
};

StructuredBuffer<Particle> particles : register(t0, space0);

cbuffer Draw : register(b0, space1)
{
    float4x4 view_projection;
    float4 right;    // xyz: the right of the camera; w: 1 stretched along the velocity
    float4 up;       // xyz: its up; w: the stretch
    float4 facing;   // xyz: toward the camera; w: the frames of the flipbook
    float4 flip;     // x: its columns; y: its rows
    float4 tint;     // the color of the emitter
    float4 sizes[8];    // 32 samples of the size over the life
    float4 colors[32];  // 32 samples of the color over the life
};

struct Output
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : TEXCOORD1;
};

float size_at(float t)
{
    float x = saturate(t) * 31.0;
    uint i = (uint)x;
    uint j = min(i + 1, 31u);
    return lerp(sizes[i >> 2][i & 3], sizes[j >> 2][j & 3], x - (float)i);
}

float4 color_at(float t)
{
    float x = saturate(t) * 31.0;
    uint i = (uint)x;
    uint j = min(i + 1, 31u);
    return lerp(colors[i], colors[j], x - (float)i);
}

Output main(uint id : SV_VertexID)
{
    Output output;
    Particle p = particles[id / 6];
    uint corners[6] = { 0, 1, 2, 0, 2, 3 };
    uint corner = corners[id % 6];
    float life = p.velocity_life.w;
    float age = p.position_age.w;
    if (life <= 0.0 || age >= life)
    {
        output.position = float4(2.0, 2.0, 2.0, 1.0);  // outside the screen
        output.uv = float2(0.0, 0.0);
        output.color = float4(0.0, 0.0, 0.0, 0.0);
        return output;
    }
    float t = age / life;
    float half_size = p.size_turn.x * size_at(t) * 0.5;
    float3 a = right.xyz * half_size;
    float3 b = up.xyz * half_size;
    if (right.w > 0.5)
    {
        float3 v = p.velocity_life.xyz;
        float3 along = v - facing.xyz * dot(v, facing.xyz);
        float l = length(along);
        if (l > 0.000001)
        {
            float3 d = along / l;
            b = d * (half_size + up.w * length(v) * 0.5);
            a = cross(d, facing.xyz) * half_size;
        }
    }
    else
    {
        float co = cos(p.size_turn.y);
        float si = sin(p.size_turn.y);
        a = (right.xyz * co + up.xyz * si) * half_size;
        b = (up.xyz * co - right.xyz * si) * half_size;
    }
    float frames = max(facing.w, 1.0);
    float columns = max(flip.x, 1.0);
    float rows = max(flip.y, 1.0);
    float frame = min(floor(t * frames), frames - 1.0);
    float u0 = fmod(frame, columns) / columns;
    float v0 = floor(frame / columns) / rows;
    float u1 = u0 + 1.0 / columns;
    float v1 = v0 + 1.0 / rows;
    float3 at = p.position_age.xyz;
    float2 uv;
    if (corner == 0) { at += -a - b; uv = float2(u0, v1); }
    else if (corner == 1) { at += a - b; uv = float2(u1, v1); }
    else if (corner == 2) { at += a + b; uv = float2(u1, v0); }
    else { at += b - a; uv = float2(u0, v0); }
    output.position = mul(view_projection, float4(at, 1.0));
    output.uv = uv;
    output.color = tint * color_at(t);
    return output;
}
