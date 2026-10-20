module;

#include <common.hxx>
#include <atomic>
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

export module volumetricclouds;

import common;
import dunia;
import settings;
import volumetricdevice;
import volumetricshaders;

// Volumetric clouds ported from the Far Cry Instincts recomp, in place of the game's flat cloud
// layer (FUN_103DC3C0, returned from at once while the option is on). Coverage comes from the two
// layers the weather turns on and their formation, light from the sun or moon, ambient from the
// cloud layer's own colour, so they follow the day and night cycle and the weather. Marched at
// quarter resolution into a history, then laid over the sky before the fog.

// Time of day state, as FUN_103DC3C0 feeds the CloudLayer shader.
static constexpr uintptr_t nLayer1Formation = 0x1C8;  // x is the share of the sky the layer covers
static constexpr uintptr_t nLayer1Enabled = 0x1DC;
static constexpr uintptr_t nLayer2Formation = 0x1E8;
static constexpr uintptr_t nLayer2Enabled = 0x1FC;
static constexpr uintptr_t nCloudAmbientColor = 0x230;
static constexpr size_t nEnvironmentSize = 0x240;

static constexpr uint32_t nNoiseSide = 128;
static constexpr double fPeriod = 24576.0;  // cloud space wraps here, every noise tile divides it
static constexpr double fWind = 6.0;        // metres a second along cloud x
static constexpr float fHistoryBlend = 0.12f;
static constexpr double fHistoryGap = 0.25;  // seconds between draws before the history is dropped
static constexpr float fHistoryMove = 100.0f;

// The ray's pixel in each 4x4 block, a different one each frame.
static constexpr uint8_t Bayer[16] = { 0, 10, 2, 8, 5, 15, 7, 13, 1, 11, 3, 9, 4, 14, 6, 12 };

static std::atomic<bool> bVolumetricClouds{ false };

static std::vector<uint32_t> vNoise;
static std::atomic<bool> bNoiseReady{ false };
static std::atomic<bool> bNoiseStarted{ false };

struct CloudHistory
{
    bool bValid = false;
    bool bParity = false;
    uint32_t nFrame = 0;
    double shift[2]{};
    LARGE_INTEGER lastDraw{};
    float last[4][4]{};  // eye, right, up, forward
    float lastProj[2]{};
};

static CloudHistory History;

// Noise, ported from the recomp's compute shader

static uint32_t Hash(uint32_t x, uint32_t y, uint32_t z)
{
    uint32_t h = (x * 73856093u) ^ (y * 19349663u) ^ (z * 83492791u);
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

struct Vec3
{
    float x;
    float y;
    float z;
};

// A point in [0, 1) of each lattice cell.
static Vec3 Cell(uint32_t x, uint32_t y, uint32_t z)
{
    return { (Hash(x, y, z) & 0xFFFFu) / 65535.0f, (Hash(x + 1013u, y, z) & 0xFFFFu) / 65535.0f,
             (Hash(x, y + 2029u, z) & 0xFFFFu) / 65535.0f };
}

// Cellular noise, 1 on the points, n cells over the tile.
static float Worley(Vec3 p, int n)
{
    const int cx = int(std::floor(p.x * n));
    const int cy = int(std::floor(p.y * n));
    const int cz = int(std::floor(p.z * n));
    const Vec3 f = { p.x * n - cx, p.y * n - cy, p.z * n - cz };
    float d = 1.0f;

    for (int i = 0; i < 27; i++)
    {
        const int ox = i % 3 - 1;
        const int oy = (i / 3) % 3 - 1;
        const int oz = i / 9 - 1;
        const auto c = Cell((cx + ox + n) % n, (cy + oy + n) % n, (cz + oz + n) % n);
        const Vec3 v = { ox + c.x - f.x, oy + c.y - f.y, oz + c.z - f.z };
        d = (std::min)(d, v.x * v.x + v.y * v.y + v.z * v.z);
    }
    return 1.0f - std::clamp(std::sqrt(d), 0.0f, 1.0f);
}

static float Lerp(float a, float b, float t) { return a + (b - a) * t; }

// Gradient noise, n cells over the tile.
static float Gradient(Vec3 p, int n)
{
    const int cx = int(std::floor(p.x * n));
    const int cy = int(std::floor(p.y * n));
    const int cz = int(std::floor(p.z * n));
    const Vec3 f = { p.x * n - cx, p.y * n - cy, p.z * n - cz };
    auto fade = [](float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); };

    float v[8];
    for (int i = 0; i < 8; i++)
    {
        const int ox = i & 1;
        const int oy = (i >> 1) & 1;
        const int oz = i >> 2;
        const auto g = Cell((cx + ox) % n, (cy + oy) % n, (cz + oz) % n);
        v[i] = (g.x * 2.0f - 1.0f) * (f.x - ox) + (g.y * 2.0f - 1.0f) * (f.y - oy) + (g.z * 2.0f - 1.0f) * (f.z - oz);
    }

    const float ux = fade(f.x);
    const float uy = fade(f.y);
    const float uz = fade(f.z);
    const float y0 = Lerp(Lerp(v[0], v[1], ux), Lerp(v[2], v[3], ux), uy);
    const float y1 = Lerp(Lerp(v[4], v[5], ux), Lerp(v[6], v[7], ux), uy);
    return Lerp(y0, y1, uz);
}

static uint32_t Byte(float v)
{
    return uint32_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
}

// Perlin worley in r, worley octaves from 4, 8 and 16 cells in g, b, a; RGBA8.
static uint32_t NoiseTexel(uint32_t x, uint32_t y, uint32_t z)
{
    const Vec3 p = { (x + 0.5f) / nNoiseSide, (y + 0.5f) / nNoiseSide, (z + 0.5f) / nNoiseSide };
    const float w4 = Worley(p, 4);
    const float w8 = Worley(p, 8);
    const float w16 = Worley(p, 16);
    const float w32 = Worley(p, 32);
    const float w64 = Worley(p, 64);
    const float fbm[3] = { w4 * 0.625f + w8 * 0.25f + w16 * 0.125f, w8 * 0.625f + w16 * 0.25f + w32 * 0.125f,
                           w16 * 0.625f + w32 * 0.25f + w64 * 0.125f };
    const float billow = std::fabs(Gradient(p, 4) + 0.5f * Gradient(p, 8) + 0.25f * Gradient(p, 16)) / 0.875f;
    const float r = Lerp(fbm[0], 1.0f, std::clamp(billow, 0.0f, 1.0f));
    return Byte(r) | Byte(fbm[0]) << 8 | Byte(fbm[1]) << 16 | Byte(fbm[2]) << 24;
}

static void FillNoiseSlice(uint32_t z)
{
    for (uint32_t y = 0; y < nNoiseSide; y++)
    {
        for (uint32_t x = 0; x < nNoiseSide; x++)
            vNoise[(size_t(z) * nNoiseSide + y) * nNoiseSide + x] = NoiseTexel(x, y, z);
    }
}

// About six seconds of one core, so split over all of them, once, off the render thread.
static void StartNoise()
{
    if (bNoiseStarted.exchange(true))
        return;

    std::thread([]()
    {
        vNoise.resize(size_t(nNoiseSide) * nNoiseSide * nNoiseSide);
        std::atomic<uint32_t> nNextSlice{ 0 };
        std::vector<std::thread> workers((std::max)(1u, std::thread::hardware_concurrency()));

        for (auto& worker : workers)
        {
            worker = std::thread([&nNextSlice]()
            {
                for (uint32_t z = nNextSlice++; z < nNoiseSide; z = nNextSlice++)
                    FillNoiseSlice(z);
            });
        }

        for (auto& worker : workers)
            worker.join();
        bNoiseReady = true;
    }).detach();
}

// Frame state

static float ReadFloat(uintptr_t nAddress)
{
    float fValue = 0.0f;
    std::memcpy(&fValue, reinterpret_cast<const void*>(nAddress), sizeof(fValue));
    return fValue;
}

static float LayerCoverage(uintptr_t nEnvironment, uintptr_t nEnabled, uintptr_t nFormation)
{
    if (*reinterpret_cast<const uint8_t*>(nEnvironment + nEnabled) == 0)
        return 0.0f;
    return std::clamp(ReadFloat(nEnvironment + nFormation), 0.0f, 1.0f);
}

// Formation x reads about a third on a partly cloudy day, which takes the full FCIP coverage.
static constexpr float fFullCoverageFormation = 0.33f;

static float Coverage(uintptr_t nEnvironment)
{
    const float fLayer1 = LayerCoverage(nEnvironment, nLayer1Enabled, nLayer1Formation);
    const float fLayer2 = LayerCoverage(nEnvironment, nLayer2Enabled, nLayer2Formation);
    return (std::min)((std::max)(fLayer1, fLayer2) / fFullCoverageFormation, 1.0f);
}

static double SecondsSince(LARGE_INTEGER from, LARGE_INTEGER now)
{
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    return double(now.QuadPart - from.QuadPart) / double(frequency.QuadPart);
}

static float Distance(const float a[4], const float b[4])
{
    const float dx = a[0] - b[0];
    const float dy = a[1] - b[1];
    const float dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Drifts the cloud space with the wind, wrapped so the floats keep their precision.
static void UpdateDrift(double fSeconds)
{
    History.shift[0] -= fWind * fSeconds;
    History.shift[0] -= fPeriod * std::floor(History.shift[0] / fPeriod);
}

// The cloud registers, and whether last frame's history lines up with this one.
static void ApplyCloudConstants(VolConstants& constants, uintptr_t nEnvironment, VolSize scene, bool bHistory)
{
    auto& c = constants.c;
    const uint32_t nRay = Bayer[History.nFrame & 15];
    const float fClock = float(GetTickCount64() % 86400000ull) / 1000.0f;

    c[kRegSize][2] = float((scene.w + 3) / 4);
    c[kRegSize][3] = float((scene.h + 3) / 4);

    const float frame[4] = { float(History.nFrame & 255), float(nRay & 3), float(nRay >> 2),
                             bHistory ? fHistoryBlend : 0.0f };
    const float shift[4] = { float(History.shift[0]), float(History.shift[1]), std::fmod(fClock / 1800.0f, 1.0f),
                             std::fmod(fClock / 240.0f, 1.0f) };
    std::memcpy(c[kRegFrame], frame, sizeof(frame));
    std::memcpy(c[kRegCloudShift], shift, sizeof(shift));
    std::memcpy(c[kRegCloudLight], reinterpret_cast<const void*>(nEnvironment + nCloudAmbientColor), sizeof(c[0]));
    c[kRegCloudLight][3] = Coverage(nEnvironment);

    std::memcpy(c[kRegPrevRight], bHistory ? History.last[1] : c[kRegRight], sizeof(c[0]));
    std::memcpy(c[kRegPrevUp], bHistory ? History.last[2] : c[kRegUp], sizeof(c[0]));
    std::memcpy(c[kRegPrevForward], bHistory ? History.last[3] : c[kRegForward], sizeof(c[0]));
    c[kRegPrevProj][0] = bHistory ? History.lastProj[0] : c[kRegProj][0];
    c[kRegPrevProj][1] = bHistory ? History.lastProj[1] : c[kRegProj][1];
}

static void LatchCamera(const VolConstants& constants)
{
    const auto& c = constants.c;
    std::memcpy(History.last[0], c[kRegEye], sizeof(c[0]));
    std::memcpy(History.last[1], c[kRegRight], sizeof(c[0]));
    std::memcpy(History.last[2], c[kRegUp], sizeof(c[0]));
    std::memcpy(History.last[3], c[kRegForward], sizeof(c[0]));
    History.lastProj[0] = c[kRegProj][0];
    History.lastProj[1] = c[kRegProj][1];
}

export bool VolumetricCloudsEnabled()
{
    return bVolumetricClouds.load(std::memory_order_relaxed);
}

export void RunVolumetricClouds(VolumetricDevice& device, VolConstants constants, uintptr_t nEnvironment)
{
    StartNoise();
    if (!bNoiseReady || !Readable(nEnvironment, nEnvironmentSize) || Coverage(nEnvironment) <= 0.0f)
    {
        History.bValid = false;
        return;
    }

    const auto scene = device.SceneSize();
    if (!device.EnsureNoise(vNoise, nNoiseSide) || !device.EnsureTarget(VolTexture::CloudsA, scene) ||
        !device.EnsureTarget(VolTexture::CloudsB, scene))
        return;

    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    const double fSeconds = History.bValid ? SecondsSince(History.lastDraw, now) : 0.0;
    const bool bHistory = History.bValid && fSeconds < fHistoryGap &&
                          Distance(History.last[0], constants.c[kRegEye]) < fHistoryMove;

    UpdateDrift((std::min)(fSeconds, fHistoryGap));
    ApplyCloudConstants(constants, nEnvironment, scene, bHistory);

    const auto eLast = History.bParity ? VolTexture::CloudsA : VolTexture::CloudsB;
    const auto eNext = History.bParity ? VolTexture::CloudsB : VolTexture::CloudsA;
    device.Draw({ VolShader::CloudMarch, eLast, eNext, VolBlend::Replace }, constants);
    device.Draw({ VolShader::CloudComposite, eNext, VolTexture::None, VolBlend::Over }, constants);

    LatchCamera(constants);
    History.bParity = !History.bParity;
    History.nFrame++;
    History.lastDraw = now;
    History.bValid = true;
}

class VolumetricClouds
{
public:
    VolumetricClouds()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            // FUN_103DC3C0, the cloud layer's parameter fill and draw: __thiscall, twelve stack arguments.
            auto* pCloudLayer = dunia_find("55 8B EC 83 E4 F0 83 EC 64 53 56 8B 75 1C 80 BE DC 01 00 00 00");
            if (!pCloudLayer)
                return;

            static raw_mem SkipCloudLayer(pCloudLayer, { 0xC2, 0x30, 0x00 });
            BindPatch(SkipCloudLayer, PREF_VOLUMETRICCLOUDS);
            BindBool(bVolumetricClouds, PREF_VOLUMETRICCLOUDS);
        };
    }
} VolumetricClouds;
