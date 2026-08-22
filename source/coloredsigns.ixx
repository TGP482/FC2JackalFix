/*
  Road and safe house signs tint towards the colour of the objective they belong to. The tint is a
  material parameter, written into the sign once when its objective changes, so a live toggle needs
  a repaint pass of its own: nothing re-runs the paint on the signs already standing in the world.
*/

module;

#include <common.hxx>
#include <atomic>

export module coloredsigns;

import common;
import dunia;
import settings;

// 0x10667B50 paints one sign. It walks the sign's mesh for the material carrying parameter
// 0x1767E828, then writes parameter 0x0E44F777 with one of four colours the sign declares. Which
// one is the index the caller passes:
//
//   10667C31  TEST ESI,ESI
//   10667C33  JL   10667C51           ; no objective, the colour at +0x20
//   10667C35  CMP  ESI,[EBP+0x70]     ; else through the MissionColors table, +0x30 past its end
//
// FUN_10667F30 registers the colour enum in index order: 0 None, 1 Main, 2 Subvert, 3 Underground,
// held at +0x20 with stride 0x10. "None" is the untinted sign, so turning the JL into a JMP is the
// whole feature. The index still reaches +0x10 on the way out, so the sign keeps the objective it
// belongs to and the repaint below can put the real colour back.
static constexpr ptrdiff_t nColourChoiceJump = 6;

// 0x10667E10 is the only thing that calls the paint, and only when the sign's stored index differs
// from the objective's. Neither end of the toggle changes an index, so the repaint walks the signs
// itself. The manager holds them in a std::set of entity references at +0xF0.
static constexpr uintptr_t nManagerSigns = 0xF0;

// CEntityRef, which is what both the manager global and each set entry hold.
static constexpr uintptr_t nRefEntity = 0x0C;

// MSVC8 _Tree node, as 0x1071BFB0 reads it.
static constexpr uintptr_t nNodeLeft = 0x00;
static constexpr uintptr_t nNodeParent = 0x04;
static constexpr uintptr_t nNodeRight = 0x08;
static constexpr uintptr_t nNodeValue = 0x18;
static constexpr uintptr_t nNodeIsNil = 0x21;

// Into the component fetch at 0x1071C005, which is the engine's own "register the tag if this is
// the first ask, then fetch" block.
static constexpr ptrdiff_t nSignTagFlag = 0x02;
static constexpr ptrdiff_t nSignTagValue = 0x11;
static constexpr ptrdiff_t nGetComponentCall = 0x17;

// Into the sign's own init at 0x10668323, for the manager reference the whole system hangs off.
static constexpr ptrdiff_t nManagerRefGlobal = 0x0E;

// Past the three INT3 pads the entry pattern needs to be unique.
static constexpr ptrdiff_t nRepaintEntry = 0x03;

// Into CPawnInputListener::Update, at the pawn fetch. See the install site.
static constexpr ptrdiff_t nInputPassPawnFetch = 0x0F;

// __thiscall on an entity. Returns the component or null.
using GetComponentByTag_t = void* (__fastcall*)(void* pEntity, void* pEdx, void* pTag);

// 0x10667C80, __thiscall on a sign. Re-runs the paint with the index the sign already holds.
using RepaintSign_t = void(__fastcall*)(void* pSign);

static GetComponentByTag_t GetComponentByTag = nullptr;
static RepaintSign_t RepaintSign = nullptr;
static void* pSignTag = nullptr;
static int32_t* pSignTagFlag = nullptr;
static void** ppManagerRef = nullptr;

// Bumped wherever the setting is applied, consumed on the engine thread. The ini watcher runs on
// its own thread and the repaint calls into the engine, so the two cannot be the same place.
static std::atomic<uint32_t> nSettingRevision{ 0 };
static uint32_t nPaintedRevision = 0;

static bool IsNilNode(uintptr_t nNode)
{
    return nNode == 0 || *reinterpret_cast<uint8_t*>(nNode + nNodeIsNil) != 0;
}

static uintptr_t ReadPtr(uintptr_t nBase, uintptr_t nOffset)
{
    return *reinterpret_cast<uintptr_t*>(nBase + nOffset);
}

static uintptr_t NextNode(uintptr_t nNode)
{
    auto nRight = ReadPtr(nNode, nNodeRight);
    if (!IsNilNode(nRight))
    {
        for (nNode = nRight; !IsNilNode(ReadPtr(nNode, nNodeLeft)); )
            nNode = ReadPtr(nNode, nNodeLeft);

        return nNode;
    }

    auto nParent = ReadPtr(nNode, nNodeParent);
    while (!IsNilNode(nParent) && nNode == ReadPtr(nParent, nNodeRight))
    {
        nNode = nParent;
        nParent = ReadPtr(nNode, nNodeParent);
    }

    return nParent;
}

static void RepaintAllSigns()
{
    if (GetComponentByTag == nullptr || RepaintSign == nullptr || ppManagerRef == nullptr)
        return;

    // The tag flag is zero until the first sign has asked for its own class, so a zero here means
    // no sign has ever loaded and nothing carries a tint.
    if (pSignTag == nullptr || pSignTagFlag == nullptr || *pSignTagFlag == 0)
        return;

    auto nManagerRef = reinterpret_cast<uintptr_t>(*ppManagerRef);
    if (nManagerRef == 0)
        return;

    auto nManager = ReadPtr(nManagerRef, nRefEntity);
    if (nManager == 0)
        return;

    auto nHead = ReadPtr(nManager, nManagerSigns);
    if (nHead == 0)
        return;

    for (auto nNode = ReadPtr(nHead, nNodeLeft); nNode != nHead && !IsNilNode(nNode); nNode = NextNode(nNode))
    {
        auto nSignRef = ReadPtr(nNode, nNodeValue);
        if (nSignRef == 0)
            continue;

        auto pEntity = reinterpret_cast<void*>(ReadPtr(nSignRef, nRefEntity));
        if (pEntity == nullptr)
            continue;

        if (auto* pSign = GetComponentByTag(pEntity, nullptr, pSignTag))
            RepaintSign(pSign);
    }
}

class ColoredSigns
{
public:
    ColoredSigns()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            if (auto* pColourChoice = dunia_find("8B 74 24 18 85 F6 7C 1C 3B 75 70 7D 11 8B 45 6C 8B 0C B0 83 C1 02 C1 E1 04 03 CD 51 EB 0A 8D 55 30 52", nColourChoiceJump))
            {
                static raw_mem fnColourChoice(pColourChoice, { 0xEB });

                ApplyAndWatch([]()
                {
                    fnColourChoice.Set(JackalFixSettings.GetInt(PREF_NOCOLOREDSIGNS) != 0);
                    nSettingRevision.fetch_add(1, std::memory_order_relaxed);
                });
            }

            // Same block debug.ixx reads for the physics tag, on CRoadSign instead: MOV ECX,EDI
            // rather than MOV ECX,ESI, and anchored into the caller's loop past the fetch so the
            // pair stays apart.
            auto tagPattern = dunia_pattern("83 3D ? ? ? ? 00 75 07 33 C9 E8 ? ? ? ? 68 ? ? ? ? 8B CF E8 ? ? ? ? 55 8B C8 E8 ? ? ? ? 80 7E 21 00");
            if (!tagPattern.empty())
            {
                auto nMatch = reinterpret_cast<uintptr_t>(tagPattern.get_first());

                pSignTagFlag = *reinterpret_cast<int32_t**>(nMatch + nSignTagFlag);
                pSignTag = *reinterpret_cast<void**>(nMatch + nSignTagValue);

                auto nFetch = nMatch + nGetComponentCall;
                GetComponentByTag = reinterpret_cast<GetComponentByTag_t>(nFetch + 5 + *reinterpret_cast<int32_t*>(nFetch + 1));
            }

            // The manager reference is read in seventeen other places with the same three
            // instructions, so the anchor is the sign init's own tail ahead of it.
            if (auto* pManagerRef = dunia_find("8B 47 08 51 8B CC 89 01 83 40 08 01 8B 1D ? ? ? ? 8B 7B 0C 83 43 08 01", nManagerRefGlobal))
                ppManagerRef = *reinterpret_cast<void***>(pManagerRef);

            if (auto* pRepaint = dunia_find("CC CC CC 8B 41 10 50 E8 ? ? ? ? C3", nRepaintEntry))
                RepaintSign = reinterpret_cast<RepaintSign_t>(pRepaint);

            // CPawnInputListener::Update, the same per frame pass debug.ixx clocks off, which is
            // where the repaint has to run: it calls into the engine and the ini watcher does not
            // own the engine thread.
            //
            //   10144AB5  MOV  ECX, [ESI+0x20]   <- hook, five bytes with the CMP behind it
            //   10144AB8  CMP  ECX, EAX
            //   10144ABA  JZ   10144B06
            //   10144ABC  CALL <get aim state>   <- debug.ixx
            //
            // Three modules share this function. lookback.ixx inline hooks the entry over six
            // bytes, debug.ixx mid hooks the call at +0x16, and this lands between them on an
            // exact instruction boundary. Widening any of the three breaks the other two.
            auto inputPassPattern = dunia_pattern("56 8B F1 74 0A 88 46 04 88 46 05 5E C2 08 00 8B 4E 20 3B C8 74 4A E8 ? ? ? ? F6 40 04 40 74 11");
            if (!inputPassPattern.empty())
            {
                static auto InputPassHook = safetyhook::create_mid(inputPassPattern.get_first(nInputPassPawnFetch), [](SafetyHookContext&)
                {
                    auto nRevision = nSettingRevision.load(std::memory_order_relaxed);
                    if (nRevision == nPaintedRevision)
                        return;

                    nPaintedRevision = nRevision;
                    RepaintAllSigns();
                });
            }
        };
    }
} ColoredSigns;
