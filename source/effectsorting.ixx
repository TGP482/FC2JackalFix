/*
Fix for hair, glasses, and water rendering over 2D elements due to sorting issues.
*/

module;

#include <common.hxx>
#include <cstdint>
#include <cstring>

export module effectsorting;

import common;
import dunia;

// ------------------------------------------------------------------------------------------------
// Hair and glasses.

static constexpr const char* pHairTechnique = "Mesh_Hair";
static constexpr const char* pSkinningDefine = "SKINNING";

// Ahead of the particle layer at 0, behind the water surface at -1000, alongside the engine's own
// blended props. Signed: the comparator tests the key with SETL.
static constexpr int32_t nCharacterSortKey = -1;

// A draw item's 64 bit shader id: the low byte is a one based index into the technique array, and
// everything from bit 8 up is one bit per registered define.
static constexpr ptrdiff_t nDrawItemShaderId = 0x10;
static constexpr uint32_t nTechniqueIndexMask = 0xFF;
static constexpr uint32_t nFirstDefineBit = 8;

// The shader manager holds both registries: techniques at +0x14 with their count at +0x18,
// defines at +0x20 with their count at +0x24. Records are pointers in the first and 0x20 byte
// entries in the second, but both carry their name as a std::string at +0x04.
static constexpr ptrdiff_t nManagerTechniques = 0x14;
static constexpr ptrdiff_t nManagerTechniqueCount = 0x18;
static constexpr ptrdiff_t nManagerDefines = 0x20;
static constexpr ptrdiff_t nManagerDefineCount = 0x24;
static constexpr size_t nDefineRecordSize = 0x20;

static constexpr ptrdiff_t nRecordName = 0x04;
static constexpr ptrdiff_t nRecordNameCapacity = 0x18;

// std::string keeps up to 15 characters inline and only allocates past that, which the capacity
// says. Same layout the game was built with.
static constexpr size_t nShortStringCapacity = 0x10;

// Into the pattern that resolves the shader manager global.
static constexpr ptrdiff_t nShaderManagerGlobal = 0x10;

// ------------------------------------------------------------------------------------------------
// Water.

// Pass ids, as CSceneRenderer::Init registers them.
static constexpr uint8_t nBlendedBeforeWater = 0x21;
static constexpr uint8_t nBlendedAfterWater = 0x22;

// Into the particle pattern: the branch into the straddling arm, and the immediate of the
// entirely-below arm.
static constexpr ptrdiff_t nStraddleBranch = 0x03;
static constexpr ptrdiff_t nBelowWaterPassId = 0x18;

// The straddling branch is a two byte JZ. Erased rather than retargeted, so the result falls into
// the decrement below it, misses the entirely-below arm too, and lands on the above-water one.
static constexpr uint8_t nShortConditionalJump = 0x74;

// Into each water pattern: the conditional that guards its depth only submission.
static constexpr ptrdiff_t nRiverPrepassBranch = 0x16;
static constexpr ptrdiff_t nSectorPrepassBranch = 0x10;

// JZ rel32 is six bytes and JMP rel32 is five, so each displacement gains one over what the
// original branch carries and the spare byte is padded.
static constexpr uint8_t nConditionalJump = 0x0F;
static constexpr uint8_t nJumpAlways = 0xE9;
static constexpr uint8_t nNop = 0x90;
static constexpr int32_t nRiverSkipDistance = 0x129;
static constexpr int32_t nSectorSkipDistance = 0xB0;

// ------------------------------------------------------------------------------------------------

static uint8_t** ppShaderManager = nullptr;

// Both registries fill as shaders load, so neither is available at init. Resolved on first use,
// with a retry only once something new has actually been registered.
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

// Defines are a flat array rather than an array of pointers, and each one owns bit (index + 8) of
// every shader id. Same arithmetic the engine's own name to bit lookup does.
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
    // Harmless in the passes that ignore the key entirely, the opaque ones sorting on render state.
    if (nSortKey > nCharacterSortKey && IsCharacterTransparent(pDrawItem))
        nSortKey = nCharacterSortKey;

    QueueDrawItemHook.fastcall(pDrawList, pEdx, pDrawItem, nSortKey, fDistance);
}

// Rewrites a six byte conditional jump into an unconditional one over the same span.
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
            // The shader manager global, read out of the CRenderPass::Execute site that resolves a
            // pass wide shader. Without it nothing matches and the hook is a compare.
            auto managerPattern = dunia_pattern("53 0B D0 8B 86 84 00 00 00 53 6A 01 0B C1 8B 0D ? ? ? ? 50 52 E8");
            if (!managerPattern.empty())
            {
                ppShaderManager = *managerPattern.get_first<uint8_t**>(nShaderManagerGlobal);

                // CRenderPassList::Add, the one function every draw list append goes through: it is
                // the only caller of the append itself, so this covers the D3D9 and D3D10 frame
                // graphs and every submitter feeding them: the plain path, the per light path, and
                // the extra copy a sub-mesh straddling a water plane gets. Entry through the three
                // argument loads, stopping before the first push.
                auto queuePattern = dunia_pattern("D9 44 24 0C 8B 44 24 08 8B 54 24 04 6A 00 6A 00 51 8B 09 D9 1C 24 50 52 E8");
                if (!queuePattern.empty())
                    QueueDrawItemHook = safetyhook::create_inline(queuePattern.get_first(), QueueDrawItem);
            }

            // CSceneParticleEmitterRenderer::Render, across the water plane classifier's return
            // value being unpicked: zero straddles, one is entirely below, anything else is above.
            // The two 0x22 stores either side of the target are the above and straddling arms.
            auto particlePattern = dunia_pattern("83 E8 00 74 19 83 E8 01 74 0A C7 44 24 2C 22 00 00 00 EB 43 C7 44 24 2C 21 00 00 00");
            if (!particlePattern.empty())
            {
                // Read back before writing, so a build whose bytes happen to line up somewhere else
                // is left alone rather than having an instruction corrupted.
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

            // CWaterRenderer::Render, at the transparent surface arm: the material flag test that
            // selects it, the render mode check, and then the prepass gate whose taken branch skips
            // straight past the depth only draw to the surface submission.
            auto waterPattern = dunia_pattern("F6 47 4C 02 74 6F 83 7C 24 60 00 0F 84 6A 03 00 00 80 7C 24 12 00 0F 84 28 01 00 00 8B 4B 04");
            if (!waterPattern.empty())
                ForceJump(waterPattern.get_first<uint8_t>(nRiverPrepassBranch), nRiverSkipDistance);

            // CSceneMeshRenderer::Submit, at its own water branch: the draw item is finished, the
            // flag that asks for a depth copy is tested, and the taken branch lands past the
            // DepthAlpha submission. The FLDZ and the pushed zero just after are the distance and
            // key a capture shows for these.
            auto sectorPattern = dunia_pattern("8B 4B 64 89 44 8B 18 83 43 64 01 80 7C 24 37 00 0F 84 AF 00 00 00 8B 8C 24 8C 00 00 00 6A 01");
            if (!sectorPattern.empty())
                ForceJump(sectorPattern.get_first<uint8_t>(nSectorPrepassBranch), nSectorSkipDistance);
        };
    }
} EffectSorting;
