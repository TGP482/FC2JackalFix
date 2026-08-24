module;

#include <common.hxx>
#include <cstdint>
#include <cstring>

export module effectsorting;

import common;
import dunia;

// Hair and glasses.

static constexpr const char* pHairTechnique = "Mesh_Hair";
static constexpr const char* pSkinningDefine = "SKINNING";

// Ahead of the particle layer at 0 and the water surface at -1000, so a body in a river is drawn
// under the surface. Signed: the comparator tests the key with SETL.
static constexpr int32_t nCharacterSortKey = -2000;

// Draw item shader id: low byte a one based technique index, bit 8 up one bit per define.
static constexpr ptrdiff_t nDrawItemShaderId = 0x10;
static constexpr uint32_t nTechniqueIndexMask = 0xFF;
static constexpr uint32_t nFirstDefineBit = 8;

// Shader manager registries: techniques at +0x14 / count +0x18 (pointers), defines at +0x20 /
// count +0x24 (0x20 byte entries). Both carry their name as a std::string at +0x04.
static constexpr ptrdiff_t nManagerTechniques = 0x14;
static constexpr ptrdiff_t nManagerTechniqueCount = 0x18;
static constexpr ptrdiff_t nManagerDefines = 0x20;
static constexpr ptrdiff_t nManagerDefineCount = 0x24;
static constexpr size_t nDefineRecordSize = 0x20;

static constexpr ptrdiff_t nRecordName = 0x04;
static constexpr ptrdiff_t nRecordNameCapacity = 0x18;

// std::string is inline up to 15 characters and only allocates past that; capacity says which.
static constexpr size_t nShortStringCapacity = 0x10;

// Into the pattern that resolves the shader manager global.
static constexpr ptrdiff_t nShaderManagerGlobal = 0x10;

// Water.

// Pass ids, as CSceneRenderer::Init registers them.
static constexpr uint8_t nBlendedBeforeWater = 0x21;
static constexpr uint8_t nBlendedAfterWater = 0x22;

// Into the particle pattern: the straddling branch, and the entirely-below arm's pass id.
static constexpr ptrdiff_t nStraddleBranch = 0x03;
static constexpr ptrdiff_t nBelowWaterPassId = 0x18;

// The straddling branch, a two byte JZ, is erased rather than retargeted, so it falls past both
// below-water arms onto the above-water one.
static constexpr uint8_t nShortConditionalJump = 0x74;

// Into each water pattern: the conditional that guards its depth only submission.
static constexpr ptrdiff_t nRiverPrepassBranch = 0x16;
static constexpr ptrdiff_t nSectorPrepassBranch = 0x10;

// JZ rel32 is six bytes, JMP rel32 five: each displacement gains one and the spare byte is padded.
static constexpr uint8_t nConditionalJump = 0x0F;
static constexpr uint8_t nJumpAlways = 0xE9;
static constexpr uint8_t nNop = 0x90;
static constexpr int32_t nRiverSkipDistance = 0x129;
static constexpr int32_t nSectorSkipDistance = 0xB0;

static uint8_t** ppShaderManager = nullptr;

// Registries fill as shaders load, so resolved on first use and retried only when a count moves.
static uint32_t nHairTechnique = 0;
static uint32_t nSkinningLow = 0;
static uint32_t nSkinningHigh = 0;
static uint32_t nSeenTechniqueCount = 0;
static uint32_t nSeenDefineCount = 0;

// __thiscall on the draw list, three stack arguments.
static SafetyHookInline QueueDrawItemHook{};

static const char* RecordName(uint8_t* pRecord)
{
    if (pRecord == nullptr)
        return nullptr;

    auto nCapacity = *reinterpret_cast<size_t*>(pRecord + nRecordNameCapacity);

    return (nCapacity < nShortStringCapacity)
        ? reinterpret_cast<const char*>(pRecord + nRecordName)
        : *reinterpret_cast<const char**>(pRecord + nRecordName);
}

// One based, matching the low byte of a shader id. Zero is the manager's own "not found".
static uint32_t FindTechnique(uint8_t* pManager, const char* pName)
{
    auto nCount = *reinterpret_cast<uint32_t*>(pManager + nManagerTechniqueCount);
    auto** ppTechniques = *reinterpret_cast<uint8_t***>(pManager + nManagerTechniques);
    if (ppTechniques == nullptr)
        return 0;

    for (uint32_t nIndex = 0; nIndex < nCount; nIndex++)
    {
        auto* pRecordName = RecordName(ppTechniques[nIndex]);
        if (pRecordName != nullptr && _stricmp(pRecordName, pName) == 0)
            return nIndex + 1;
    }

    return 0;
}

// Defines are a flat array, each owning bit (index + 8) of every shader id.
static uint64_t FindDefineBit(uint8_t* pManager, const char* pName)
{
    auto nCount = *reinterpret_cast<uint32_t*>(pManager + nManagerDefineCount);
    auto* pDefines = *reinterpret_cast<uint8_t**>(pManager + nManagerDefines);
    if (pDefines == nullptr)
        return 0;

    for (uint32_t nIndex = 0; nIndex < nCount; nIndex++)
    {
        auto* pRecordName = RecordName(pDefines + static_cast<size_t>(nIndex) * nDefineRecordSize);
        if (pRecordName != nullptr && _stricmp(pRecordName, pName) == 0)
            return 1ULL << (nIndex + nFirstDefineBit);
    }

    return 0;
}

static void Resolve()
{
    auto* pManager = (ppShaderManager != nullptr) ? *ppShaderManager : nullptr;
    if (pManager == nullptr)
        return;

    auto nTechniques = *reinterpret_cast<uint32_t*>(pManager + nManagerTechniqueCount);
    if (nHairTechnique == 0 && nTechniques != nSeenTechniqueCount)
    {
        nSeenTechniqueCount = nTechniques;
        nHairTechnique = FindTechnique(pManager, pHairTechnique);
    }

    auto nDefines = *reinterpret_cast<uint32_t*>(pManager + nManagerDefineCount);
    if ((nSkinningLow | nSkinningHigh) == 0 && nDefines != nSeenDefineCount)
    {
        nSeenDefineCount = nDefines;

        auto nBit = FindDefineBit(pManager, pSkinningDefine);
        nSkinningLow = static_cast<uint32_t>(nBit);
        nSkinningHigh = static_cast<uint32_t>(nBit >> 32);
    }
}

static bool IsCharacterTransparent(uint8_t* pDrawItem)
{
    if (pDrawItem == nullptr)
        return false;

    if (nHairTechnique == 0 || (nSkinningLow | nSkinningHigh) == 0)
        Resolve();

    auto nLow = *reinterpret_cast<uint32_t*>(pDrawItem + nDrawItemShaderId);
    auto nHigh = *reinterpret_cast<uint32_t*>(pDrawItem + nDrawItemShaderId + 4);

    if (((nLow & nSkinningLow) | (nHigh & nSkinningHigh)) != 0)
        return true;

    return nHairTechnique != 0 && (nLow & nTechniqueIndexMask) == nHairTechnique;
}

static void __fastcall QueueDrawItem(void* pDrawList, void* pEdx, uint8_t* pDrawItem, int32_t nSortKey, float fDistance)
{
    // Clamped rather than assigned, so a material that already asks to draw further back keeps it.
    if (nSortKey > nCharacterSortKey && IsCharacterTransparent(pDrawItem))
        nSortKey = nCharacterSortKey;

    QueueDrawItemHook.fastcall(pDrawList, pEdx, pDrawItem, nSortKey, fDistance);
}

// Six byte conditional jump rewritten to an unconditional one over the same span.
static void ForceJump(uint8_t* pBranch, int32_t nDistance)
{
    if (pBranch == nullptr || injector::ReadMemory<uint8_t>(pBranch, true) != nConditionalJump)
        return;

    injector::WriteMemory<uint8_t>(pBranch, nJumpAlways, true);
    injector::WriteMemory<int32_t>(pBranch + 1, nDistance, true);
    injector::WriteMemory<uint8_t>(pBranch + 5, nNop, true);
}

class EffectSorting
{
public:
    EffectSorting()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            // Shader manager global, read out of the site that resolves a pass wide shader.
            if (auto* pManagerSite = dunia_find("53 0B D0 8B 86 84 00 00 00 53 6A 01 0B C1 8B 0D ? ? ? ? 50 52 E8", nShaderManagerGlobal))
            {
                ppShaderManager = *static_cast<uint8_t***>(pManagerSite);

                // CRenderPassList::Add, the only caller of the draw list append; entry through the
                // three argument loads.
                if (auto* pQueue = dunia_find("D9 44 24 0C 8B 44 24 08 8B 54 24 04 6A 00 6A 00 51 8B 09 D9 1C 24 50 52 E8"))
                    QueueDrawItemHook = safetyhook::create_inline(pQueue, QueueDrawItem);
            }

            // CSceneParticleEmitterRenderer::Render, on the water plane classifier's result: zero
            // straddles, one entirely below, anything else above.
            auto particlePattern = dunia_pattern("83 E8 00 74 19 83 E8 01 74 0A C7 44 24 2C 22 00 00 00 EB 43 C7 44 24 2C 21 00 00 00");
            if (!particlePattern.empty())
            {
                // Read back before writing, so a build that happens to line up is left alone.
                auto* pPassId = particlePattern.get_first<uint8_t>(nBelowWaterPassId);
                if (injector::ReadMemory<uint8_t>(pPassId, true) == nBlendedBeforeWater)
                    injector::WriteMemory<uint8_t>(pPassId, nBlendedAfterWater, true);

                auto* pStraddle = particlePattern.get_first<uint8_t>(nStraddleBranch);
                if (injector::ReadMemory<uint8_t>(pStraddle, true) == nShortConditionalJump)
                {
                    injector::WriteMemory<uint8_t>(pStraddle, nNop, true);
                    injector::WriteMemory<uint8_t>(pStraddle + 1, nNop, true);
                }
            }

            // CWaterRenderer::Render transparent arm, at the prepass gate: taken, it skips the
            // depth only draw and lands on the surface submission.
            if (auto* pRiver = dunia_find("F6 47 4C 02 74 6F 83 7C 24 60 00 0F 84 6A 03 00 00 80 7C 24 12 00 0F 84 28 01 00 00 8B 4B 04", nRiverPrepassBranch))
                ForceJump(static_cast<uint8_t*>(pRiver), nRiverSkipDistance);

            // CSceneMeshRenderer::Submit water branch: taken, it lands past DepthAlpha.
            if (auto* pSector = dunia_find("8B 4B 64 89 44 8B 18 83 43 64 01 80 7C 24 37 00 0F 84 AF 00 00 00 8B 8C 24 8C 00 00 00 6A 01", nSectorPrepassBranch))
                ForceJump(static_cast<uint8_t*>(pSector), nSectorSkipDistance);
        };
    }
} EffectSorting;
