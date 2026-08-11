/*
  "Removing coloured road signs" from An Almost Complete Guide to Far Cry 2 Modding, in code.

  The guide's version is a data edit: every MissionObjectiveSigns.* prototype in
  entitylibrarypatchoverride.fcb carries a "Colors" block (hash C512C6A9) with four Vector4s, None
  (DFA2AFF1), Main (1F1A625A), Subvert (7D65CCD1) and Underground (590E69F7). The fix rewrites the
  last three to match None, 0000803F x4, and needs a repacked patch.dat.

  Those four are the "Colors" property group of the CRoadSign component, registered in FUN_10667f30
  as children 0..3, all at offset 0x20 with indices 0..3. The shared accessor (FUN_10667860)
  computes object + [desc+0xC] + [desc+0x10] * 0x10, so the block is a flat Vector4[4] at
  CRoadSign+0x20 through +0x5F. The constructor (FUN_106679d0, vtable 0x10E8D988) seeds Colors[0]
  white and Colors[1..3] red before the entity library overwrites all four.

  Only FUN_10667b50(CRoadSign* this, int nTagIndex) reads them, reached from FUN_10667e10 when the
  sign's active objective tag changes and from the refresh thunk at 0x10667C80. It is a three-way
  select fed to CMaterialInstance::SetVector4 (FUN_10409E60) as material parameter 0x0E44F777:

    TEST ESI, ESI               ; nTagIndex
    JL   colour_none            ; no objective    -> Colors[0] at [EBP+0x20]
    CMP  ESI, [EBP+0x70]        ; MissionColors count
    JGE  colour_main            ; tag not in list -> Colors[1] at [EBP+0x30]
    ...                         ; else this + 0x20 + enumColor * 0x10, enumColor from [EBP+0x6C]

  Turning the JL into an unconditional JMP (0x7C -> 0xEB, same length, same target) sends every path
  to Colors[0], the untinted white entry both by the constructor's default and in the shipped data.

  ESI is deliberately left alone: the store two instructions past the call, MOV [EBP+0x10], ESI, is
  the component's CurrentTag bookkeeping. Clobbering it to -1 to force the existing JL would make
  FUN_10667e10 think the tag never settles and re-run this for every sign, every tick.

  Toggling live works, but signs already tinted keep their tint until their objective tag next
  changes. Starting or finishing any mission clears them.
*/

module;

#include <common.hxx>

export module coloredsigns;

import common;
import dunia;
import settings;

// Repainting the signs that are already up.
//
// The patch decides what colour a sign is given, but a sign is only ever given one when something
// asks: the objective tag moving, or the sign streaming in. Left at that, changing the setting
// recoloured nothing until the next mission event, which is what "not live" meant here.
//
// The engine keeps the signs in a std::map on a manager entity and walks it itself every time the
// tag moves (the loop at Dunia+71BFB0). The same walk is done below, ending at the property event
// the engine registers for CRoadSign, a ten byte thunk that re-runs the colour select against the
// tag the sign is already carrying:
//
//     8B 41 10   MOV EAX, [ECX+10h]      ; CurrentTag
//     50         PUSH EAX
//     E8 ...     CALL <colour select>
//     C3         RET
//
// It has no dirty check, so it repaints whether or not anything changed, which is what a toggle
// needs in both directions.

// The manager is reached through a ref-counted holder; the manager entity itself is at +0Ch.
static constexpr ptrdiff_t nHolderObject   = 0x0C;
static constexpr ptrdiff_t nManagerMapHead = 0xF0;   // std::map _Myhead
static constexpr ptrdiff_t nNodeLeft       = 0x00;
static constexpr ptrdiff_t nNodeParent     = 0x04;
static constexpr ptrdiff_t nNodeRight      = 0x08;
static constexpr ptrdiff_t nNodeValue      = 0x18;   // the entity's reference proxy
static constexpr ptrdiff_t nNodeIsNil      = 0x21;
static constexpr ptrdiff_t nProxyObject    = 0x0C;   // null once the entity has gone

// CRoadSign's mesh sub-component. The colour select dereferences it with no test of its own, and
// the constructor leaves it unset on its allocation-failure path, so it is tested here instead.
static constexpr ptrdiff_t nSignMeshes     = 0x78;

// A bound on the walk, so a manager pointer that is not one cannot loop forever.
static constexpr size_t nMaxSigns = 8192;

using SignRefresh_t    = void  (__fastcall*)(void* pComponent, void* pEdx);
using GetComponent_t   = void* (__fastcall*)(void* pEntity, void* pEdx, const void* pClassName);
using SignTagChanged_t = void  (__fastcall*)(void* pComponent, void* pEdx, void* pTag);

static void**         ppSignManagerHolder = nullptr;
static const void*    pRoadSignClassName  = nullptr;
static SignRefresh_t  SignRefresh         = nullptr;
static GetComponent_t GetComponentByType  = nullptr;

static SafetyHookInline SignTagChangedHook{};

// The thread the engine drives the signs from, learned the first time it does. The map is that
// thread's and is walked from nowhere else.
static std::atomic<DWORD> nSignThread = 0;

// Set when the setting moved somewhere the walk cannot be done, the ini file watcher having a
// thread of its own. Drained below, from a place the engine only ever calls on its own thread.
static std::atomic<bool> bSignsStale = false;

static void RefreshAllSigns()
{
    if (ppSignManagerHolder == nullptr || SignRefresh == nullptr || GetComponentByType == nullptr
        || pRoadSignClassName == nullptr)
    {
        return;
    }

    auto* pHolder = static_cast<uint8_t*>(*ppSignManagerHolder);
    if (pHolder == nullptr)
    {
        return;
    }

    auto* pManager = *reinterpret_cast<uint8_t**>(pHolder + nHolderObject);
    if (pManager == nullptr)
    {
        return;
    }

    auto* pHead = *reinterpret_cast<uint8_t**>(pManager + nManagerMapHead);
    if (pHead == nullptr)
    {
        return;
    }

    auto IsNil = [](uint8_t* pNode) { return pNode == nullptr || *(pNode + nNodeIsNil) != 0; };

    size_t nSeen = 0;
    size_t nPainted = 0;

    auto* pNode = *reinterpret_cast<uint8_t**>(pHead + nNodeLeft); // begin()
    for (size_t nGuard = 0; nGuard < nMaxSigns && pNode != nullptr && pNode != pHead; nGuard++)
    {
        nSeen++;
        if (auto* pProxy = *reinterpret_cast<uint8_t**>(pNode + nNodeValue))
        {
            // The engine's own walk erases nodes whose entity has gone; this one only steps over
            // them, since nothing here is allowed to change the map it is standing in.
            if (auto* pEntity = *reinterpret_cast<void**>(pProxy + nProxyObject))
            {
                auto* pComponent = static_cast<uint8_t*>(GetComponentByType(pEntity, nullptr, pRoadSignClassName));
                if (pComponent != nullptr && *reinterpret_cast<void**>(pComponent + nSignMeshes) != nullptr)
                {
                    SignRefresh(pComponent, nullptr);
                    nPainted++;
                }
            }
        }

        // In-order successor, the ordinary red-black walk, the same one the engine does two
        // instructions past its own call.
        auto* pRight = *reinterpret_cast<uint8_t**>(pNode + nNodeRight);
        if (!IsNil(pRight))
        {
            pNode = pRight;
            for (auto* pLeft = *reinterpret_cast<uint8_t**>(pNode + nNodeLeft); !IsNil(pLeft);
                 pLeft = *reinterpret_cast<uint8_t**>(pNode + nNodeLeft))
            {
                pNode = pLeft;
            }
            continue;
        }

        auto* pParent = *reinterpret_cast<uint8_t**>(pNode + nNodeParent);
        while (!IsNil(pParent) && pNode == *reinterpret_cast<uint8_t**>(pParent + nNodeRight))
        {
            pNode = pParent;
            pParent = *reinterpret_cast<uint8_t**>(pNode + nNodeParent);
        }
        pNode = pParent;
    }

}

// The engine's per-sign tag handler. It is taken over only as a place to stand: it runs on the game
// thread, once per sign as one streams in and once per sign whenever the objective tag moves, which
// is often enough that a change made by editing the ini lands within moments.
static void __fastcall SignTagChanged(void* pComponent, void* pEdx, void* pTag)
{
    nSignThread = GetCurrentThreadId();

    SignTagChangedHook.fastcall(pComponent, pEdx, pTag);

    if (bSignsStale.exchange(false))
        RefreshAllSigns();
}

class ColoredSigns
{
public:
    ColoredSigns()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            // Tag load through the last of the three colour branches. The parameter hash push that
            // follows is the first absolute address, so it is left out.
            auto pattern = dunia_pattern("8B 74 24 18 85 F6 7C 1C 3B 75 70 7D 11 8B 45 6C 8B 0C B0 83 C1 02 C1 E1 04 03 CD 51 EB 0A 8D 55 30 52 EB 04 8D 45 20 50");
            if (pattern.empty())
                return;

            // JL colour_none -> JMP colour_none
            static raw_mem fnSignColorSelect(pattern.get_first(6), { 0xEB });

            // The middle of the engine's own walk over every sign: it resolves the class name, asks
            // the entity for its CRoadSign and hands that the current tag. All three things the
            // repaint needs are inside those five instructions.
            auto walkPattern = dunia_pattern("33 C9 E8 ? ? ? ? 68 ? ? ? ? 8B CF E8 ? ? ? ? 55 8B C8 E8 ? ? ? ? 80 7E 21 00");
            if (!walkPattern.empty())
            {
                auto* pSite = walkPattern.get_first<uint8_t>();

                pRoadSignClassName = *reinterpret_cast<const void* const*>(pSite + 8);
                GetComponentByType = reinterpret_cast<GetComponent_t>(pSite + 19 + *reinterpret_cast<int32_t*>(pSite + 15));

                auto* pTagChanged = pSite + 27 + *reinterpret_cast<int32_t*>(pSite + 23);
                SignTagChangedHook = safetyhook::create_inline(pTagChanged, SignTagChanged);
            }

            // The holder the manager hangs off, taken from the one site that loads it, takes a
            // reference and asks the manager entity for a component in the same breath.
            auto holderPattern = dunia_pattern("8B 35 ? ? ? ? 83 46 08 01 8B DE 8B 7B 0C 83 C6 08 8B CF E8 ? ? ? ? 8D 54 24 4C");
            if (!holderPattern.empty())
                ppSignManagerHolder = *holderPattern.get_first<void**>(2);

            // The property event registered for CRoadSign, which re-runs the colour select against
            // the tag the sign already has.
            auto refreshPattern = dunia_pattern("8B 41 10 50 E8 ? ? ? ? C3");
            if (!refreshPattern.empty())
                SignRefresh = reinterpret_cast<SignRefresh_t>(refreshPattern.get_first());

            static auto ColoredSignsCB = []()
            {
                if (JackalFixSettings.GetInt(PREF_NOCOLOREDSIGNS) != 0)
                    fnSignColorSelect.Write();
                else
                    fnSignColorSelect.Restore();
            };

            ColoredSignsCB();

            JackalFix::onIniFileChange() += []()
            {
                ColoredSignsCB();

                // Straight away when the change came from the menu, which applies on the game's own
                // thread; queued when it came from an edit to the file, which does not.
                const auto nThread = nSignThread.load();
                if (nThread != 0 && GetCurrentThreadId() == nThread)
                {
                    RefreshAllSigns();
                }
                else
                {
                    bSignsStale = true;
                }
            };
        };
    }
} ColoredSigns;
