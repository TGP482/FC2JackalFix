module;

#include <cstddef>

export module volumetricshaders;

// Pixel shader constant registers, as the HLSL below declares them.
export enum VolRegister : size_t
{
    kRegSize,
    kRegProj,
    kRegEye,
    kRegRight,
    kRegUp,
    kRegForward,
    kRegDepth,
    kRegFog,
    kRegFogHeight,
    kRegFogColor,
    kRegLightDir,
    kRegLightColor,
    kRegBlur,
    kRegFogColorVector,
    kRegFogColorRange,
    kRegExposure,
    nVolRegisters,
};

// One source for both renderers: SM4 is compiled with backwards compatibility, so sampler2D and
// tex2Dlod bind texture and sampler N together, matching SM3's sN.
export constexpr const char* szVolumetricShaders = R"(
#ifdef SM4
#define TARGET SV_Target
#define BEGIN_CONSTANTS cbuffer Volumetrics : register(b0) {
#define END_CONSTANTS };
#define REG(n) : packoffset(c##n)
#else
#define TARGET COLOR0
#define BEGIN_CONSTANTS
#define END_CONSTANTS
#define REG(n) : register(c##n)
#endif

#ifdef VERTEX
#ifdef SM4
void FullscreenVS(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0)
{
    uv = float2((id << 1) & 2, id & 2);
    pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
#else
float4 g_HalfPixel : register(c0);
void FullscreenVS(float2 corner : POSITION, out float4 pos : POSITION, out float2 uv : TEXCOORD0)
{
    uv = corner;
    pos = float4(corner * float2(2.0, -2.0) + float2(-1.0, 1.0) + g_HalfPixel.xy, 0.0, 1.0);
}
#endif
#else

BEGIN_CONSTANTS
float4 g_Size REG(0);        // scene w, h, low res target w, h
float4 g_Proj REG(1);        // P00, P11, near, far
float4 g_Eye REG(2);
float4 g_Right REG(3);
float4 g_Up REG(4);
float4 g_Forward REG(5);
float4 g_Depth REG(6);       // view depth = dot(rgb, xyz), sky from w
float4 g_Fog REG(7);         // FogValues: 1 / (end - start), -start / (end - start), most
float4 g_FogHeight REG(8);   // FogHeightValues: height ramp scale, offset, then amount span, base
float4 g_FogColor REG(9);
float4 g_LightDir REG(10);   // direction the sun or moon light travels
float4 g_LightColor REG(11);
float4 g_Blur REG(12);       // tap step in uv
float4 g_FogColorVector REG(13);
float4 g_FogColorRange REG(14);
float4 g_Exposure REG(15);   // BloomAdaptationFactor, which the game's lighting and fog are scaled by
END_CONSTANTS

sampler2D g_DepthTex : register(s0);
sampler2D g_Input : register(s1);

static const float kFogReach = 250.0;
static const int kFogSteps = 16;
// Thinner than FCIP: the game's own distance fog is already in the scene, and with no sun shadow
// map the forward glow is not broken up by trees, so it is held near the fog colour's brightness.
static const float kFogDensity = 0.5;
static const float kSunScatter = 0.15;

float4 Fetch(sampler2D s, float2 uv) { return tex2Dlod(s, float4(uv, 0.0, 0.0)); }

float SceneDepth(float2 uv)
{
    float d = dot(Fetch(g_DepthTex, uv).rgb, g_Depth.xyz);
    return d >= g_Depth.w ? 1e6 : d;
}

float Noise(float2 p) { return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715)))); }
float2 Ndc(float2 uv) { return uv * float2(2.0, -2.0) + float2(-1.0, 1.0); }
float2 ViewSlope(float2 uv) { return Ndc(uv) / g_Proj.xy; }

// Unit view depth along it.
float3 ViewRay(float2 uv)
{
    float2 v = ViewSlope(uv);
    return v.x * g_Right.xyz + v.y * g_Up.xyz + g_Forward.xyz;
}

// The game's fog colour along a ray, as its vertex shaders blend it: FogColor towards FogColorVector,
// plus FogColorRange by the horizontal angle away from it (acos / pi).
float3 FogColorAlong(float3 dir)
{
    float2 h = dir.xy / max(length(dir.xy), 1e-4);
    float t = acos(clamp(dot(h, g_FogColorVector.xy), -1.0, 1.0)) / 3.14159265;
    return g_FogColor.rgb + t * g_FogColorRange.rgb;
}

// The game's height fog amount at height z, 1 where the level has none.
float HeightFog(float z) { return g_FogHeight.w + g_FogHeight.z * saturate(z * g_FogHeight.x + g_FogHeight.y); }

#ifdef FOG_MARCH
// In scatter in rgb, transmittance in a.
float4 main(float2 uv : TEXCOORD0) : TARGET
{
    float depth = SceneDepth(uv);
    if (depth <= 0.0)
        return float4(0.0, 0.0, 0.0, 1.0);
    float3 dir = ViewRay(uv);
    float len = length(dir);
    float end = min(depth, kFogReach / len);
    float mu = -dot(dir / len, g_LightDir.xyz);
    float phase = 0.4 + 0.6 * 0.75 / pow(1.25 - mu, 1.5);
    float3 sun = g_LightColor.rgb * phase * kSunScatter * g_Exposure.x;
    // The game's fog colour is where its own fog settles, so the march never darkens past it.
    float3 ambient = FogColorAlong(dir) * g_Exposure.x;
    float sigma = max(g_Fog.x, 2e-4) * saturate(g_Fog.z) * kFogDensity;
    float jitter = Noise(floor(uv * g_Size.zw));
    float3 light = 0.0;
    float trans = 1.0;
    [loop] for (int i = 0; i < kFogSteps; i++)
    {
        float s = (i + jitter) / kFogSteps;
        float3 p = g_Eye.xyz + dir * (end * s * s);
        float ext = sigma * HeightFog(p.z) * end * (2.0 * i + 1.0) / (kFogSteps * kFogSteps) * len;
        light += trans * ext * (sun + ambient);
        trans *= exp(-ext);
    }
    return float4(light, trans);
}
#endif

#ifdef FOG_BLUR
// Depth aware 9 tap blur along g_Blur.xy.
float4 main(float2 uv : TEXCOORD0) : TARGET
{
    float c = SceneDepth(uv);
    float4 sum = 0.0;
    float weight = 0.0;
    [unroll] for (int i = -4; i <= 4; i++)
    {
        float2 q = uv + g_Blur.xy * i;
        float w = exp(-0.1 * i * i) * max(0.0, 1.0 - abs(SceneDepth(q) - c) / (c * 0.05 + 1e-3));
        sum += Fetch(g_Input, q) * w;
        weight += w;
    }
    return sum / max(weight, 1e-4);
}
#endif

#ifdef FOG_COMPOSITE
// A march over length a taken to length b, as if the fog along it were even.
float4 Stretch(float4 f, float a, float b)
{
    float k = b / max(a, 1e-3);
    float t = pow(max(f.a, 1e-6), k);
    float e = 1.0 - f.a;
    return float4(e > 1e-3 ? f.rgb * (1.0 - t) / e : f.rgb * k, t);
}

// Depth aware upsample of the four half res neighbours, each stretched to this depth, so a thin
// blade in front of the sky does not take the sky's fog.
float4 main(float2 uv : TEXCOORD0) : TARGET
{
    float depth = SceneDepth(uv);
    if (depth <= 0.0)
        return float4(0.0, 0.0, 0.0, 1.0);
    float2 v = ViewSlope(uv);
    float cap = kFogReach / sqrt(1.0 + dot(v, v));
    float2 hp = uv * g_Size.zw - 0.5;
    float2 base = floor(hp);
    float2 f = hp - base;
    float4 sum = 0.0;
    float weight = 1e-6;
    [unroll] for (int i = 0; i < 4; i++)
    {
        float2 o = float2(i % 2, i / 2);
        float2 q = (base + o + 0.5) / g_Size.zw;
        float2 bw = lerp(1.0 - f, f, o);
        float lin = SceneDepth(q);
        float w = bw.x * bw.y * exp(-abs(lin - depth) / (0.05 * depth + 1e-3)) + 1e-5;
        sum += Stretch(Fetch(g_Input, q), min(lin, cap), min(depth, cap)) * w;
        weight += w;
    }
    return sum / weight;
}
#endif

#endif
)";
