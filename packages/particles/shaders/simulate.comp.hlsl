// The compute shader of particles.compute: each particle of the buffer one step later, or born
// from what the processor sent. SDL_GPU: sampled textures and read-only storage buffers in space0,
// read-write storage buffers in space1, uniform buffers in space2.
//
// A particle: position and age, velocity and life, size at birth, rotation, spin, where it reads
// the noise. A life of 0 is a free place.

struct Particle
{
    float4 position_age;
    float4 velocity_life;
    float4 size_turn;
};

Texture2D<float> scene_depth : register(t0, space0);
SamplerState depth_sampler : register(s0, space0);
StructuredBuffer<Particle> born : register(t1, space0);
RWStructuredBuffer<Particle> particles : register(u0, space1);

cbuffer Step : register(b0, space2)
{
    float4x4 view_projection;
    float4x4 inverse_view_projection;
    float4 camera;      // xyz: where it is; w: how thick what the depth shows is, in meters
    float4 wind;        // xyz: the wind; w: the drag
    float4 forces;      // x: gravity (m/s², down); y: turbulence; z: its scale; w: the time
    float4 contact;     // x: bounce; y: friction; z: 1 to die on contact; w: 1 to collide with the depth
    float4 counts;      // x: dt; y: the places of the buffer; z: where the born go; w: how many
    float4 planes[4];   // normal, offset
    float4 extra;       // x: the planes; y, z: a pixel of the depth, in uv
    float4 speeds[8];   // 32 samples of the speed over the life
};

float curve_at(float t)
{
    float x = saturate(t) * 31.0;
    uint i = (uint)x;
    uint j = min(i + 1, 31u);
    float a = speeds[i >> 2][i & 3];
    float b = speeds[j >> 2][j & 3];
    return lerp(a, b, x - (float)i);
}

float lattice(int3 c, uint salt)
{
    uint h = ((uint)c.x * 73856093u) ^ ((uint)c.y * 19349663u) ^ ((uint)c.z * 83492791u) ^ (salt * 2654435761u);
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return (float)(h & 0xFFFFFFu) / 8388607.5 - 1.0;
}

float noise(float3 p, uint salt)
{
    float3 f = floor(p);
    int3 c = (int3)f;
    float3 t = p - f;
    t = t * t * (3.0 - 2.0 * t);
    float total = 0.0;
    for (uint k = 0; k < 8; k++)
    {
        int3 d = int3(k & 1, (k >> 1) & 1, (k >> 2) & 1);
        float w = (d.x == 1 ? t.x : 1.0 - t.x) * (d.y == 1 ? t.y : 1.0 - t.y) * (d.z == 1 ? t.z : 1.0 - t.z);
        total += w * lattice(c + d, salt);
    }
    return total;
}

// The point of the scene seen at `uv`, at the depth `d`.
float3 unproject(float2 uv, float d)
{
    float4 w = mul(inverse_view_projection, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, d, 1.0));
    return w.xyz / w.w;
}

float3 surface_at(float2 uv)
{
    return unproject(uv, scene_depth.SampleLevel(depth_sampler, uv, 0));
}

void bounce_on(inout Particle p, float3 n)
{
    float into = dot(p.velocity_life.xyz, n);
    if (into < 0.0)
    {
        float3 along = p.velocity_life.xyz - n * into;
        p.velocity_life.xyz = along * (1.0 - contact.y) - n * (into * contact.x);
    }
}

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint capacity = (uint)counts.y;
    if (id.x >= capacity)
    {
        return;
    }
    uint head = (uint)counts.z;
    uint count = (uint)counts.w;
    uint after = (id.x + capacity - head) % capacity;
    if (after < count)
    {
        particles[id.x] = born[after];
        return;
    }
    Particle p = particles[id.x];
    float dt = counts.x;
    if (p.velocity_life.w <= 0.0 || p.position_age.w >= p.velocity_life.w)
    {
        return;
    }
    p.position_age.w += dt;
    if (p.position_age.w >= p.velocity_life.w)
    {
        particles[id.x] = p;
        return;
    }
    float3 v = p.velocity_life.xyz + float3(0.0, -forces.x, 0.0) * dt;
    if (wind.w > 0.0)
    {
        v += (wind.xyz - v) * min(wind.w * dt, 1.0);
    }
    if (forces.y > 0.0)
    {
        float3 q = p.position_age.xyz / max(forces.z, 0.0001) + float3(p.size_turn.w, forces.w * 0.5, 0.0);
        v += float3(noise(q, 1u), noise(q, 2u), noise(q, 3u)) * (forces.y * dt);
    }
    float pace = curve_at(p.position_age.w / p.velocity_life.w);
    p.velocity_life.xyz = v;
    p.position_age.xyz += v * (pace * dt);
    p.size_turn.y += p.size_turn.z * dt;
    bool landed = false;
    uint planes_count = (uint)extra.x;
    for (uint k = 0; k < planes_count && k < 4; k++)
    {
        float3 n = normalize(planes[k].xyz);
        float depth = dot(n, p.position_age.xyz) - planes[k].w;
        if (depth < 0.0)
        {
            landed = true;
            p.position_age.xyz -= n * depth;
            bounce_on(p, n);
        }
    }
    if (contact.w > 0.5)
    {
        float4 clip = mul(view_projection, float4(p.position_age.xyz, 1.0));
        if (clip.w > 0.0001)
        {
            float3 ndc = clip.xyz / clip.w;
            if (abs(ndc.x) < 1.0 && abs(ndc.y) < 1.0 && ndc.z < 1.0)
            {
                float2 uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
                float d = scene_depth.SampleLevel(depth_sampler, uv, 0);
                if (d < 1.0 && ndc.z > d)
                {
                    float3 surface = unproject(uv, d);
                    if (distance(surface, p.position_age.xyz) < camera.w)
                    {
                        // the normal of what it hit, from the depth of the pixels beside
                        float3 sx = surface_at(uv + float2(extra.y, 0.0));
                        float3 sy = surface_at(uv + float2(0.0, extra.z));
                        float3 n = cross(sx - surface, sy - surface);
                        float l = length(n);
                        n = l > 0.000001 ? n / l : normalize(camera.xyz - surface);
                        if (dot(n, camera.xyz - surface) < 0.0)
                        {
                            n = -n;
                        }
                        landed = true;
                        p.position_age.xyz = surface + n * 0.01;
                        bounce_on(p, n);
                    }
                }
            }
        }
    }
    if (landed && contact.z > 0.5)
    {
        p.position_age.w = p.velocity_life.w;
    }
    particles[id.x] = p;
}
