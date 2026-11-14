// The fragment shader of render3d: physically based light (metal and roughness, as glTF), from a
// sun that casts shadows and up to four point lights, plus an ambient light. Colors are linear;
// the result is written for a screen (gamma 2.2), and the texture of base color read as sRGB.

cbuffer Lights : register(b0, space3)
{
    float4 camera_position;
    float4 sun_direction;      // xyz: where the light goes; w: its intensity
    float4 sun_color;          // rgb; w: 1 when it casts shadows
    float4 ambient;            // rgb
    float4 point_position[4];  // xyz; w: the range
    float4 point_color[4];     // rgb; w: the intensity
    float4 counts;             // x: the point lights; y: 1 / the size of the map of shadows
};

cbuffer Material : register(b1, space3)
{
    float4 base_color;
    float4 factors;  // x: metallic; y: roughness; z: 1 when there is a texture
};

Texture2D<float4> albedo : register(t0, space2);
SamplerState albedo_sampler : register(s0, space2);
Texture2D<float> shadows : register(t1, space2);
SamplerComparisonState shadow_sampler : register(s1, space2);

static const float PI = 3.14159265;

float distribution(float n_h, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float d = n_h * n_h * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 1e-6);
}

float geometry(float n_v, float n_l, float roughness)
{
    float k = (roughness + 1.0) * (roughness + 1.0) / 8.0;
    return (n_v / (n_v * (1.0 - k) + k)) * (n_l / (n_l * (1.0 - k) + k));
}

float3 fresnel(float cos_theta, float3 f0)
{
    return f0 + (1.0 - f0) * pow(1.0 - cos_theta, 5.0);
}

// The light that leaves toward the eye from a light coming from `l`, of `radiance`.
float3 shade(float3 n, float3 v, float3 l, float3 radiance, float3 albedo_color, float metallic, float roughness)
{
    float3 h = normalize(v + l);
    float n_l = max(dot(n, l), 0.0);
    float n_v = max(dot(n, v), 1e-4);
    float3 f0 = lerp(float3(0.04, 0.04, 0.04), albedo_color, metallic);
    float3 f = fresnel(max(dot(h, v), 0.0), f0);
    float3 specular = distribution(max(dot(n, h), 0.0), roughness) * geometry(n_v, n_l, roughness) * f
        / max(4.0 * n_v * n_l, 1e-4);
    float3 diffuse = (1.0 - f) * (1.0 - metallic) * albedo_color / PI;
    return (diffuse + specular) * radiance * n_l;
}

// How much of the sun reaches the point, from 0 to 1: nine samples of the map of shadows.
float lit_by_sun(float4 light, float n_l)
{
    if (sun_color.w < 0.5)
    {
        return 1.0;
    }
    float3 p = light.xyz / light.w;
    float2 uv = p.xy * float2(0.5, -0.5) + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || p.z > 1.0)
    {
        return 1.0;  // beyond the map: lit
    }
    float bias = max(0.004 * (1.0 - n_l), 0.0008);
    float sum = 0.0;
    for (int y = -1; y <= 1; y++)
    {
        for (int x = -1; x <= 1; x++)
        {
            sum += shadows.SampleCmpLevelZero(shadow_sampler, uv + float2(x, y) * counts.y, p.z - bias);
        }
    }
    return sum / 9.0;
}

float4 main(float3 world : TEXCOORD0, float3 normal : TEXCOORD1, float2 uv : TEXCOORD2, float4 light : TEXCOORD3) : SV_Target0
{
    float4 color = base_color;
    if (factors.z > 0.5)
    {
        float4 texel = albedo.Sample(albedo_sampler, uv);
        color *= float4(pow(texel.rgb, 2.2), texel.a);  // sRGB to linear
    }
    float metallic = saturate(factors.x);
    float roughness = clamp(factors.y, 0.04, 1.0);
    float3 n = normalize(normal);
    float3 v = normalize(camera_position.xyz - world);
    float3 result = ambient.rgb * color.rgb;

    float3 to_sun = normalize(-sun_direction.xyz);
    float n_l = max(dot(n, to_sun), 0.0);
    result += shade(n, v, to_sun, sun_color.rgb * sun_direction.w, color.rgb, metallic, roughness) * lit_by_sun(light, n_l);

    int lights = (int)counts.x;
    for (int i = 0; i < 4; i++)
    {
        if (i >= lights)
        {
            break;
        }
        float3 to_light = point_position[i].xyz - world;
        float distance = length(to_light);
        float range = max(point_position[i].w, 1e-3);
        float fade = saturate(1.0 - distance / range);
        float3 radiance = point_color[i].rgb * point_color[i].w * fade * fade / max(distance * distance, 1e-2);
        result += shade(n, v, to_light / max(distance, 1e-4), radiance, color.rgb, metallic, roughness);
    }
    return float4(pow(max(result, 0.0), 1.0 / 2.2), color.a);
}
