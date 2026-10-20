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
    kRegFrame,
    kRegCloudShift,
    kRegCloudLight,
    kRegPrevRight,
    kRegPrevUp,
    kRegPrevForward,
    kRegPrevProj,
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
float4 g_Frame REG(16);      // frame, ray offset in the 4x4 block, history weight (0 without one)
float4 g_CloudShift REG(17); // world xy to cloud xy, weather and detail noise slices
float4 g_CloudLight REG(18); // the cloud layer's ambient colour, coverage
float4 g_PrevRight REG(19);  // last frame's camera
float4 g_PrevUp REG(20);
float4 g_PrevForward REG(21);
float4 g_PrevProj REG(22);   // last frame's P00, P11
END_CONSTANTS

sampler2D g_DepthTex : register(s0);
sampler2D g_Input : register(s1);

static const float kFogReach = 250.0;
static const int kFogSteps = 16;
// Thinner than FCIP: the game's own distance fog is already in the scene, and with no sun shadow
// map the forward glow is not broken up by trees, so it is held near the fog colour's brightness.
static const float kFogDensity = 0.6;
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


#if defined(CLOUD_MARCH) || defined(CLOUD_COMPOSITE)
// A layer high over the land, marched at quarter resolution into a history, then laid over the sky.
static const float kBase = 1500.0;  // over the ground, metres
static const float kTop = 2700.0;
static const float kShapeTile = 3072.0;  // noise periods, each divides the cloud space wrap
static const float kDetailTile = 384.0;
static const float kWeatherTile = 24576.0;
static const float kFar = 40000.0;  // no layer entered beyond
static const float kSpan = 8000.0;  // longest stretch marched
static const float kSigma = 0.05;   // extinction at full density, per metre
// Optical depth of the horizon haze looking straight up. FC2's distance fog is far too dense to stand
// for it (FogValues.x 0.006, the land fogs out by 170 m) and the sky dome is drawn without it.
static const float kHaze = 0.15;
static const int kSteps = 40;

sampler3D g_Noise : register(s3);  // perlin worley, worley octaves from 4, 8 and 16 cells

float Remap(float v, float a, float b, float c, float d) { return c + (v - a) * (d - c) / (b - a); }
bool Finite(float4 v) { return all(v == v) && all(abs(v) < 1e30); }

// Base and top, at least a kilometre over the camera.
float2 Layer()
{
    float base = max(0.0, g_Eye.z - 500.0) + kBase;
    return float2(base, base + kTop - kBase);
}

float3 RayDir(float2 pixel) { return normalize(ViewRay(pixel / g_Size.xy)); }

// Where a ray from the camera enters and leaves the layer, x >= y when it misses.
float2 LayerHit(float3 dir)
{
    float2 t = (Layer() - g_Eye.z) / max(dir.z, 1e-4);
    return dir.z > 0.0 && t.x < kFar ? float2(t.x, min(t.y, t.x + kSpan)) : float2(1.0, 0.0);
}
#endif

#ifdef CLOUD_MARCH
float4 Noise3(float3 p) { return tex3Dlod(g_Noise, float4(p, 0.0)); }

// Clouded share of the sky at cloud xy, up to 0.6; g_CloudLight.w scales it by the weather.
float Coverage(float2 xy)
{
    float w = Noise3(float3(xy / kWeatherTile, g_CloudShift.z)).r;
    return saturate(Remap(w, 0.45, 0.8, 0.0, 1.0)) * 0.6 * g_CloudLight.w;
}

// Density at cloud space c (z over the base), hf the height in the layer, before the detail.
float Shape(float3 c, float hf, float cov)
{
    float4 n = Noise3(c / kShapeTile);
    float low = dot(n.gba, float3(0.625, 0.25, 0.125));
    float shape = saturate((saturate(Remap(n.r, low - 1.0, 1.0, 0.0, 1.0)) - 0.6) / 0.28);
    float top = 0.35 + 1.0833 * cov;
    shape *= saturate(hf * 10.0) * saturate((top - hf) / (0.6 * top));
    return saturate(Remap(shape, 1.0 - cov, 1.0, 0.0, 1.0)) * cov;
}

// Detail eats the edges: wisps at the bottom, billows on top.
float Erode(float d, float3 c, float hf, float amount)
{
    float3 n = Noise3(c / kDetailTile + float3(0.0, 0.0, g_CloudShift.w)).gba;
    float f = dot(n, float3(0.625, 0.25, 0.125));
    f = lerp(f, 1.0 - f, saturate(hf * 4.0));
    return saturate(Remap(d, f * amount, 1.0, 0.0, 1.0));
}

// Henyey Greenstein over the isotropic phase.
float Hg(float g, float mu) { return (1.0 - g * g) / pow(max(1.0 + g * g - 2.0 * g * mu, 1e-4), 1.5); }

// Optical depth toward the light over 30, 60, 120 and 240 m steps, cheap density.
float LightDepth(float3 c, float3 toLight, float thick, float cov)
{
    float depth = 0.0;
    float len = 30.0;
    [unroll] for (int j = 0; j < 4; j++)
    {
        c += toLight * len;
        float hf = c.z / thick;
        if (hf < 1.0)
            depth += Shape(c, hf, cov) * len;
        len *= 2.0;
    }
    return depth * kSigma;
}

// Horizon haze: low clouds take the fog colour and fade into the sky, never past the level's most
// fog. Dark skies fade them.
float4 Haze(float3 dir, float3 scatter, float trans)
{
    float most = saturate(g_Fog.z);
    float path = kHaze / max(dir.z, 0.02);
    float haze = most * (1.0 - exp(-path));
    float a = 1.0 - trans;
    float3 fog = FogColorAlong(dir);
    scatter = lerp(scatter, fog * a, haze);
    float lum = dot(g_LightColor.rgb + fog, float3(0.3, 0.59, 0.11));
    float keep = saturate(dir.z * 12.0) * (1.0 - haze * haze) * saturate(lum * 3.0);
    return float4(scatter * keep * g_Exposure.x, 1.0 - a * keep);
}

// In scatter in rgb, transmittance in a, along the unit ray dir over [t.x, t.y].
float4 March(float3 dir, float2 t, float jitter)
{
    float2 layer = Layer();
    float thick = layer.y - layer.x;
    float3 start = float3(g_Eye.xy + g_CloudShift.xy, g_Eye.z - layer.x);
    float3 toLight = -g_LightDir.xyz;
    bool lit = dot(g_LightColor.rgb, float3(1.0, 1.0, 1.0)) > 1e-3;
    float mu = dot(dir, toLight);
    float3 lightColor = g_LightColor.rgb * (0.8 * lerp(Hg(0.6, mu), Hg(-0.25, mu), 0.3));
    float silver = saturate(mu * 0.5 + 0.5);
    float ds = (t.y - t.x) / kSteps;
    float detail = 0.35 * saturate(1.0 - t.x / 12000.0);
    float3 scatter = 0.0;
    float trans = 1.0;
    [loop] for (int i = 0; i < kSteps && trans > 0.03; i++)
    {
        float3 c = start + dir * (t.x + (i + jitter) * ds);
        float hf = c.z / thick;
        float cov = Coverage(c.xy);
        float d = cov > 0.0 ? Shape(c, hf, cov) : 0.0;
        if (d <= 0.0)
            continue;
        d = Erode(d, c, hf, detail);
        if (d <= 0.0)
            continue;
        float sigma = d * kSigma;
        float3 light = g_CloudLight.rgb * (0.45 + 0.45 * saturate(hf));
        if (lit)
        {
            float depth = LightDepth(c, toLight, thick, cov);
            float beer = max(exp(-depth), 0.4 * exp(-0.25 * depth));
            float powder = lerp(1.0 - exp(-sigma * 80.0), 1.0, silver);
            light += lightColor * beer * powder;
        }
        float st = exp(-sigma * ds);
        scatter += trans * light * (1.0 - st);
        trans *= st;
    }
    return Haze(dir, scatter, trans);
}

// Last frame's history where the direction was then, over the texels that marched. a < 0 without one.
float4 History(float3 dir)
{
    if (g_Frame.w <= 0.0)
        return -1.0;
    float3 v = float3(dot(dir, g_PrevRight.xyz), dot(dir, g_PrevUp.xyz), dot(dir, g_PrevForward.xyz));
    if (v.z < 1e-3)
        return -1.0;
    float2 ndc = v.xy / v.z * g_PrevProj.xy;
    float2 q = (ndc * float2(0.5, -0.5) + 0.5) * g_Size.zw - 0.5;
    float2 base = floor(q);
    float2 f = q - base;
    float4 sum = 0.0;
    float weight = 0.0;
    [unroll] for (int i = 0; i < 4; i++)
    {
        float2 o = float2(i % 2, i / 2);
        float2 p = base + o;
        float4 h = Fetch(g_Input, (p + 0.5) / g_Size.zw);
        float2 bw = lerp(1.0 - f, f, o);
        bool inside = all(p >= 0.0) && all(p < g_Size.zw);
        float w = inside && h.a >= 0.0 && Finite(h) ? bw.x * bw.y + 1e-4 : 0.0;
        sum += h * w;
        weight += w;
    }
    return weight > 0.0 ? sum / weight : -1.0;
}

// One ray per 4x4 block through this frame's pixel of it, blended into last frame's history seen
// through the block centre. Blocks with nothing past the layer's entry hold a below 0.
float4 main(float2 uv : TEXCOORD0) : TARGET
{
    float2 q = floor(uv * g_Size.zw);
    float2 block = q * 4.0;
    float3 centre = RayDir(block + 2.0);
    float farthest = 0.0;
    [unroll] for (int i = 0; i < 4; i++)
        farthest = max(farthest, SceneDepth((block + float2(i % 2, i / 2) * 2.0 + 1.0) / g_Size.xy));

    float2 v = ViewSlope((block + 2.0) / g_Size.xy);
    if (!(farthest * sqrt(1.0 + dot(v, v)) >= LayerHit(centre).x))
        return float4(0.0, 0.0, 0.0, -1.0);

    float3 dir = RayDir(block + g_Frame.yz + 0.5);
    float2 t = LayerHit(dir);
    float jitter = frac(Noise(q) + g_Frame.x * 0.618034);
    float4 cur = t.x < t.y ? March(dir, t, jitter) : float4(0.0, 0.0, 0.0, 1.0);
    if (!Finite(cur))
        cur = float4(0.0, 0.0, 0.0, 1.0);
    float4 last = History(centre);
    return last.a >= 0.0 ? lerp(last, cur, g_Frame.w) : cur;
}
#endif

#ifdef CLOUD_COMPOSITE
// Over the sky and whatever lies past the layer's entry: the texels around the pixel that marched.
float4 main(float2 uv : TEXCOORD0) : TARGET
{
    float depth = SceneDepth(uv);
    float2 v = ViewSlope(uv);
    float2 t = LayerHit(RayDir(uv * g_Size.xy));
    if (t.x >= t.y || depth * sqrt(1.0 + dot(v, v)) < t.x)
        return float4(0.0, 0.0, 0.0, 1.0);
    float2 q = uv * g_Size.zw - 0.5;
    float2 base = floor(q);
    float2 f = q - base;
    float4 sum = 0.0;
    float weight = 0.0;
    [unroll] for (int i = 0; i < 4; i++)
    {
        float2 o = float2(i % 2, i / 2);
        float4 c = Fetch(g_Input, (base + o + 0.5) / g_Size.zw);
        float2 bw = lerp(1.0 - f, f, o);
        float w = c.a >= 0.0 ? bw.x * bw.y + 1e-4 : 0.0;
        sum += c * w;
        weight += w;
    }
    return weight > 0.0 ? sum / weight : float4(0.0, 0.0, 0.0, 1.0);
}
#endif

#endif
)";
