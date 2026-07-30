/*
  Restores the gameplay HUD controller button prompts. The menu nav bar is a separate system. A fix
  for it was written and tested, then reverted because the menus are wanted as they ship, so the
  notes below are a record and nothing here acts on them.

  Neither system is disabled in code. There is no platform check and no pad-connected flag; Dunia
  never works out whether a pad is present at all. CInputDriverGamepad::Poll throws away
  XInputGetState's return code, ERROR_DEVICE_NOT_CONNECTED is compared nowhere in the image, and
  the only XInput imports are XInputGetState and XInputSetState. FUN_105362E0 already resolves
  ui/360.mgb into a valid sprite on stock PC.

  The menus fail twice. FUN_101D26D0 builds icon attribute names with the literal "_pc" suffix,
  and across every shipped .mgb.desc there are 300 icon_xenon, 300 icon_ps3, 140 show_pc, zero
  icon_pc and zero bare icon, so the lookup misses on every prompt. Pointing that one lookup at
  "_xenon" fixes it. Do not touch show_pc, which has 140 real uses. Second, FUN_10189BA0 pushes
  the sprite in only after finding a child named "i_placeholder", the glyph Image the PC art pass
  deleted. What survives is three other children:

      "action"         crc 0x47CC8C92   magma::Placeholder   invisible, draws nothing
      "i_background"   crc 0x7A8A6532   magma::Image         the pill behind the label
      "t_button_text"  crc 0x49D78B9C   magma::Text          the label

  "action" is the console build's glyph anchor. A Placeholder draws nothing but is still a Widget,
  so it carries a state with a rect; in shipped data that rect reads back empty, so a square off
  the pill's own height, 24x24, is the working fallback. The reverted fix borrowed i_background as
  the carrier, hooked FUN_10189BA0 (CNavBarPrompt::SetIcon, attached flag at +0x51, element at
  +0x08) and swapped the suffix operand at rebuild time from CNavBarModule::SetLayout.

  The gameplay HUD fails differently. hud.mgb.desc declares four button slots:

      <Interact_prompt   path="a_interact_object/a_interact_icon_anim/a_prompt_interact"   text="t_button" />
      <Inventory_prompt  path="a_inventory_object/a_inventory_icons_anim/a_prompt_interact" text="t_button" />
      <Reload_prompt     path="a_ammo_object/a_reload_icon_object/a_reload_icon_anims"      text="t_button" />
      <Swap_prompt       path="a_weapon_switch_object/a_weapon_switch/a_swap_icon"          text="t_button" />

  None of those names, nor t_button, exist as strings in Dunia.dll. They fall through a generic
  loop in CHud::BuildFromLayout that reads only "path" and appends the widget to a vector at
  HUD+0x338 whose sole consumer is a bulk hide. Console fills them by token substitution (xex
  0x8296D130, 0x82973D50) and the PC hud.mgb has no a_prompt_interact container, so the group
  FUN_1086B670 shows on a prompt is on screen and empty.

  Buttons come from the shipped console action map: use -> pad:a and pad:y, reload -> pad:x,
  tryuseied -> pad:right_trigger, heal -> pad:left_shoulder. Console interacts with Y.

  magma layout, which every offset below depends on:

      Widget + 0x08          -> State
      Widget + 0x0C          -> component lock mask, bit set = engine must not write
      Widget + 0x3C          -> child Area          (AreaInstance and friends)
      Area   + 0x28 / + 0x2C -> child node vector, begin and end, stride 4
      node   + 0x08          -> zlib CRC-32 of the child's name; no strings are kept
      node   + 0x14          -> the drawable
      State  + 0x24/26/28/2A -> int16 left / right / top / bottom
      Image  + 0x20          -> sprite

  Type identity is exact vtable equality. magma::Image has no subclasses and its ObjectTypeInfo
  vtable has exactly one xref, so no engine call is needed to identify one.

  A rect written into State+0x24 does not survive a frame: RectState::blend rewrites it from the
  .mgb every tick unless the matching bit is set in Widget+0x0C. 0xF00 pins the rectangle, 0xE0
  rotation and pivot, 0x0F800000 the vertex colours. Everything else keeps blending, so the
  prompts still fade with the HUD.
*/

module;

#include <common.hxx>
#include <cstdint>

export module buttonprompts;

import common;
import dunia;
import settings;
import inputdevice;

static bool bControllerPrompts = true;

// --------------------------------------------------------------------------------------------
// magma object layout
// --------------------------------------------------------------------------------------------

static constexpr ptrdiff_t nWidgetState = 0x08;
static constexpr ptrdiff_t nWidgetLockMask = 0x0C;
static constexpr ptrdiff_t nWidgetChildArea = 0x3C;

static constexpr ptrdiff_t nAreaChildBegin = 0x28;
static constexpr ptrdiff_t nAreaChildEnd = 0x2C;

static constexpr ptrdiff_t nNodeNameId = 0x08;
static constexpr ptrdiff_t nNodeDrawable = 0x14;

static constexpr ptrdiff_t nStateLeft = 0x24;
static constexpr ptrdiff_t nStateRight = 0x26;
static constexpr ptrdiff_t nStateTop = 0x28;
static constexpr ptrdiff_t nStateBottom = 0x2A;

static constexpr ptrdiff_t nImageSprite = 0x20;

static constexpr ptrdiff_t nStateRotation = 0x18;
static constexpr ptrdiff_t nStatePivotX = 0x1C;
static constexpr ptrdiff_t nStatePivotY = 0x1E;

// The four vertex colours. The ink blot is faint, so a glyph inheriting them is barely visible.
static constexpr ptrdiff_t nStateColour = 0x44;
static constexpr int nColourCount = 4;
static constexpr uint32_t nOpaqueWhite = 0xFFFFFFFF;

// Component mask bits, from RotationState::blend and RectState::blend. Set means "do not blend".
// 5-7 rotation angle and pivot, 8-11 left/right/top/bottom, 23-27 vertex colours.
static constexpr uint32_t nRotationLockBits = 0x000000E0;
static constexpr uint32_t nRectLockBits = 0x00000F00;
static constexpr uint32_t nColourLockBits = 0x0F800000;

static constexpr ptrdiff_t nImageVtableRva = 0xEE6A04;
static constexpr ptrdiff_t nAreaInstanceVtableRva = 0xEE6BB4;

static uintptr_t Rva(ptrdiff_t nOffset)
{
    return reinterpret_cast<uintptr_t>(hDunia) + nOffset;
}

static uint8_t* GetState(uint8_t* pWidget)
{
    return pWidget ? *reinterpret_cast<uint8_t**>(pWidget + nWidgetState) : nullptr;
}

struct Rect
{
    int16_t nLeft = 0;
    int16_t nRight = 0;
    int16_t nTop = 0;
    int16_t nBottom = 0;

    bool Valid() const { return nRight > nLeft && nBottom > nTop; }

    bool Same(const Rect& o) const
    {
        return nLeft == o.nLeft && nRight == o.nRight && nTop == o.nTop && nBottom == o.nBottom;
    }
};

static Rect ReadRect(uint8_t* pState)
{
    Rect rect;
    if (!pState)
        return rect;

    rect.nLeft = *reinterpret_cast<int16_t*>(pState + nStateLeft);
    rect.nRight = *reinterpret_cast<int16_t*>(pState + nStateRight);
    rect.nTop = *reinterpret_cast<int16_t*>(pState + nStateTop);
    rect.nBottom = *reinterpret_cast<int16_t*>(pState + nStateBottom);
    return rect;
}

static void WriteRect(uint8_t* pState, const Rect& rect)
{
    if (!pState)
        return;

    *reinterpret_cast<int16_t*>(pState + nStateLeft) = rect.nLeft;
    *reinterpret_cast<int16_t*>(pState + nStateRight) = rect.nRight;
    *reinterpret_cast<int16_t*>(pState + nStateTop) = rect.nTop;
    *reinterpret_cast<int16_t*>(pState + nStateBottom) = rect.nBottom;
}

// FUN_10AA7150 is bit exact zlib CRC-32: reflected 0xEDB88320, init and final complement.
static uint32_t NameId(std::string_view sName)
{
    static constexpr uint32_t nPolynomial = 0xEDB88320;

    static const auto table = []()
    {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i)
        {
            auto c = i;
            for (int nBit = 0; nBit < 8; ++nBit)
                c = (c & 1) ? (nPolynomial ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
        return t;
    }();

    uint32_t nCrc = 0xFFFFFFFF;
    for (auto ch : sName)
        nCrc = (nCrc >> 8) ^ table[static_cast<uint8_t>(static_cast<uint8_t>(ch) ^ nCrc)];

    return ~nCrc;
}

// Walks an Area's direct children. The 64 cap is a sanity bound on a vector that may be garbage,
// not a format limit, so raising it buys nothing.
template <typename F>
static void ForEachChild(uint8_t* pArea, F&& fn)
{
    if (!pArea)
        return;

    auto ppBegin = *reinterpret_cast<uint8_t***>(pArea + nAreaChildBegin);
    auto ppEnd = *reinterpret_cast<uint8_t***>(pArea + nAreaChildEnd);
    if (!ppBegin || ppEnd < ppBegin)
        return;

    auto nCount = static_cast<size_t>(ppEnd - ppBegin);
    if (nCount > 64)
        return;

    for (size_t i = 0; i < nCount; ++i)
    {
        auto pNode = ppBegin[i];
        if (!pNode)
            continue;

        auto nId = *reinterpret_cast<uint32_t*>(pNode + nNodeNameId);
        auto pDrawable = *reinterpret_cast<uint8_t**>(pNode + nNodeDrawable);
        if (pDrawable)
            fn(nId, pDrawable);
    }
}

static bool IsImage(uint8_t* pDrawable)
{
    return pDrawable && *reinterpret_cast<uintptr_t*>(pDrawable) == Rva(nImageVtableRva);
}

// An AreaInstance carries its sub tree on its own child Area, so descending means one more hop.
static uint8_t* GetSubArea(uint8_t* pDrawable)
{
    if (!pDrawable || *reinterpret_cast<uintptr_t*>(pDrawable) != Rva(nAreaInstanceVtableRva))
        return nullptr;

    return *reinterpret_cast<uint8_t**>(pDrawable + nWidgetChildArea);
}

// The engine's own resolver descends through widget instances rather than child names, so a
// middle component like a_prompt_interact is not in the runtime tree. Search for the leaf.
static uint8_t* FindNamedDeep(uint8_t* pArea, uint32_t nWanted, int nMaxDepth = 4, int nDepth = 0)
{
    if (!pArea || nDepth > nMaxDepth)
        return nullptr;

    uint8_t* pFound = nullptr;

    ForEachChild(pArea, [&](uint32_t nId, uint8_t* pDrawable)
    {
        if (pFound)
            return;

        if (nId == nWanted)
            pFound = pDrawable;
        else if (auto pSub = GetSubArea(pDrawable))
            pFound = FindNamedDeep(pSub, nWanted, nMaxDepth, nDepth + 1);
    });

    return pFound;
}

static uint8_t* FindNamedDeepParent(uint8_t* pArea, uint32_t nWanted, uint8_t** ppParent, int nMaxDepth = 8, int nDepth = 0)
{
    if (!pArea || nDepth > nMaxDepth)
        return nullptr;

    uint8_t* pFound = nullptr;

    ForEachChild(pArea, [&](uint32_t nId, uint8_t* pDrawable)
    {
        if (pFound)
            return;

        if (nId == nWanted)
        {
            pFound = pDrawable;
            if (ppParent)
                *ppParent = pArea;
        }
        else if (auto pSub = GetSubArea(pDrawable))
        {
            pFound = FindNamedDeepParent(pSub, nWanted, ppParent, nMaxDepth, nDepth + 1);
        }
    });

    return pFound;
}

// Depth first, first match wins. Callers pass a subtree they have already narrowed down, so the
// first Image cannot turn out to be an unrelated icon.
static uint8_t* FindImage(uint8_t* pArea, int nDepth = 0)
{
    if (!pArea || nDepth > 4)
        return nullptr;

    uint8_t* pFound = nullptr;

    ForEachChild(pArea, [&](uint32_t, uint8_t* pDrawable)
    {
        if (pFound)
            return;

        if (IsImage(pDrawable))
            pFound = pDrawable;
        else if (auto pSub = GetSubArea(pDrawable))
            pFound = FindImage(pSub, nDepth + 1);
    });

    return pFound;
}

// --------------------------------------------------------------------------------------------
// Saved state, so returning to mouse and keyboard puts everything back
// --------------------------------------------------------------------------------------------

struct SavedImage
{
    uintptr_t nSprite = 0;
    uintptr_t nGlyph = 0;
    Rect rect;
    float fRotation = 0.0f;
    int16_t nPivotX = 0;
    int16_t nPivotY = 0;
    uint32_t nColours[nColourCount] = {};
    uint32_t nLockMask = 0;
    bool bHeld = false;

    // Where the glyph currently sits. Compared against each new ComputeGlyphRect result.
    Rect placed;
    bool bPlaced = false;
};

static std::map<uint8_t*, SavedImage> mSavedImages;

static bool ShouldShowGlyphs()
{
    return bControllerPrompts && IsPadActiveDevice();
}

// --------------------------------------------------------------------------------------------
// Building the widget the PC assets are missing
// --------------------------------------------------------------------------------------------

/*
  The PC hud.mgb has the a_prompt_interact container and its t_button child deleted, so there is
  no widget to put a glyph in. Ask magma's factory for one instead of editing the archive:

      Factory::CreateElementForType(ti)  0xABF0E0  Element plus drawable, with a constructed
                                                   State. The raw Image factory leaves State as
                                                   uninitialised heap.
      Factory::CreateKeyframe(ti)        0xABEE80
      Element::AddKeyframe(kf)           0xAB1C10
      Area::AddElement(e)                vtable +0x48, read at runtime
      Area::SetTime(0,0,0)               0xA973E0  forces one evaluation

  The Area destructor frees the Element and its drawable, so one from our own CRT would hand a
  foreign pointer to magma's pool free.

  The keyframe is not optional: an empty keyframe vector gets index 0xFFF ("none") and the draw
  gate refuses to draw it. Contents do not matter, the components are pinned afterwards.

  These are raw offsets rather than patterns because each is an absolute the code only reaches
  through a global. All are image base relative, so a rebase is still handled.
*/

/*
  There is no backdrop behind the button. The console texture is not in the PC archive and tinting
  a white one reads as a smudge. Reaching a sprite in another document, if one ever turns up, is
  FUN_105355B0 __thiscall(ECX = [0x11645C3C], std::string*) -> Document* then
  FUN_105361A0 __stdcall(Document*, const char*) -> Sprite*. The first wants a std::string header
  built on the stack (+0x04 buffer, +0x14 size, +0x18 capacity; under 16 chars never allocates).
*/

static constexpr ptrdiff_t nFactoryGlobalRva = 0x1664768;
static constexpr ptrdiff_t nImageTypeInfoRva = 0x1663F0C;
static constexpr ptrdiff_t nCreateElementRva = 0xABF0E0;
static constexpr ptrdiff_t nCreateKeyframeRva = 0xABEE80;
static constexpr ptrdiff_t nAddKeyframeRva = 0xAB1C10;
static constexpr ptrdiff_t nAreaSetTimeRva = 0xA973E0;

static constexpr ptrdiff_t nElementName = 0x08;
static constexpr ptrdiff_t nElementDrawable = 0x14;
static constexpr ptrdiff_t nKeyframeTime = 0x18;
static constexpr ptrdiff_t nAreaAddElementSlot = 0x48;

using CreateElement_t = uint8_t*(__thiscall*)(void*, void*);
using CreateKeyframe_t = uint8_t*(__thiscall*)(void*, void*);
using AddKeyframe_t = void(__thiscall*)(uint8_t*, uint8_t*);
using AreaAddElement_t = void(__thiscall*)(uint8_t*, uint8_t*);
using AreaSetTime_t = void(__thiscall*)(uint8_t*, int32_t, int32_t, int32_t);

// One per Area: rebuilding every frame would leak until the document unloads.
static std::map<std::pair<uint8_t*, uint32_t>, uint8_t*> mBuiltSlots;

static uint8_t* BuildImageChild(uint8_t* pArea, uint32_t nName)
{
    if (!pArea)
        return nullptr;

    auto key = std::make_pair(pArea, nName);

    auto it = mBuiltSlots.find(key);
    if (it != mBuiltSlots.end())
        return it->second;

    // Remembered even on failure, so a broken attempt is made once rather than every frame.
    mBuiltSlots[key] = nullptr;

    auto pFactory = *reinterpret_cast<void**>(Rva(nFactoryGlobalRva));
    if (!pFactory)
        return nullptr;

    auto pImageType = reinterpret_cast<void*>(Rva(nImageTypeInfoRva));

    auto pElement = reinterpret_cast<CreateElement_t>(Rva(nCreateElementRva))(pFactory, pImageType);
    if (!pElement)
        return nullptr;

    auto pDrawable = *reinterpret_cast<uint8_t**>(pElement + nElementDrawable);
    if (!pDrawable || !IsImage(pDrawable) || !GetState(pDrawable))
        return nullptr;

    *reinterpret_cast<uint32_t*>(pElement + nElementName) = nName;

    auto pKeyframe = reinterpret_cast<CreateKeyframe_t>(Rva(nCreateKeyframeRva))(pFactory, pImageType);
    if (!pKeyframe)
        return nullptr;

    *reinterpret_cast<uint16_t*>(pKeyframe + nKeyframeTime) = 0;
    reinterpret_cast<AddKeyframe_t>(Rva(nAddKeyframeRva))(pElement, pKeyframe);

    auto ppVtable = *reinterpret_cast<void***>(pArea);
    if (!ppVtable)
        return nullptr;

    reinterpret_cast<AreaAddElement_t>(ppVtable[nAreaAddElementSlot / sizeof(void*)])(pArea, pElement);
    reinterpret_cast<AreaSetTime_t>(Rva(nAreaSetTimeRva))(pArea, 0, 0, 0);

    mBuiltSlots[key] = pDrawable;
    return pDrawable;
}

// --------------------------------------------------------------------------------------------
// Gameplay HUD
// --------------------------------------------------------------------------------------------

// FUN_105362E0. One stack argument, RET 4, so __stdcall.
using GetPadButtonImage_t = uintptr_t(__stdcall*)(int32_t);
static GetPadButtonImage_t GetPadButtonImage = nullptr;

// The second glyph set in ui\textures\360\ (360_a_shop and friends) is a dead end: flat monochrome
// outlines for the weapon shop. The "orange" console interact prompt is just Y being yellow.

static constexpr ptrdiff_t nHudPromptName = 0x04;
static constexpr ptrdiff_t nHudPromptRefCount = 0x08;
static constexpr ptrdiff_t nHudPromptArea = 0x0C;
static constexpr ptrdiff_t nHudPromptUpdateCall = 14;

// CHud fields. The update loop holds the prompt vector, and the widget root hangs off the HUD.
static constexpr ptrdiff_t nHudPromptVector = 0x244;
static constexpr ptrdiff_t nHudRootArea = 0x11C;

// Xbox button ids as FUN_10533F10 numbers them: A 0, B 1, X 2, Y 3, LT 4, RT 5, LB 6, RB 7.
static constexpr int32_t nPadX = 2;
static constexpr int32_t nPadY = 3;
static constexpr int32_t nPadRT = 5;
static constexpr int32_t nPadLB = 6;

// One HUD prompt. szAbove names both the sibling the new widget is added next to and the rect
// everything is measured from. szSlot is the fallback carrier if the widget cannot be built.
struct HudPrompt
{
    const char* szName;
    int32_t nButton;
    const char* szSlot[2];  // carrier candidates, first one found wins
    const char* szAbove;    // sit the glyph above this sibling, if it is there
    const char* szObject;   // the prompt's container, searched from the HUD root
    const char* szDesigned; // the slot hud.mgb.desc names, if it turns out to exist
};

// The glyph is a square above its icon, as on 360, sized from that icon since the prompts vary.
static constexpr int nGlyphScalePercent = 85;
static constexpr int nGlyphGapPercent = 20;

static constexpr std::array<HudPrompt, 8> sHudPrompts =
{{
    { "interact",     nPadY,  { "stain",   nullptr }, "i_use_icon", "a_interact_object",      "a_prompt_interact"   },
    { "watch",        nPadY,  { "stain",   nullptr }, "i_watch",    "a_inventory_object",     "a_prompt_interact"   },
    { "ratchet",      nPadY,  { "stain",   nullptr }, "i_wrench",   "a_inventory_object",     "a_prompt_interact"   },
    { "map",          nPadY,  { "stain",   nullptr }, "i_watch",    "a_inventory_object",     "a_prompt_interact"   },
    { "ied",          nPadRT, { "stain",   nullptr }, "i_ied",      "a_inventory_object",     "a_prompt_interact"   },
    { "syringe",      nPadLB, { "stain",   nullptr }, "i_watch",    "a_inventory_object",     "a_prompt_interact"   },
    { "reload",       nPadX,  { "stain",   nullptr }, nullptr,      "a_ammo_object",          "a_reload_icon_anims" },
    { "switchweapon", nPadY,  { "i_stain", "stain" }, "i_arrow",    "a_weapon_switch_object", "a_swap_icon"         },
}};

static const HudPrompt* FindHudPrompt(uint32_t nName)
{
    static const auto ids = []()
    {
        std::array<uint32_t, sHudPrompts.size()> v{};
        for (size_t i = 0; i < sHudPrompts.size(); ++i)
            v[i] = NameId(sHudPrompts[i].szName);
        return v;
    }();

    for (size_t i = 0; i < sHudPrompts.size(); ++i)
    {
        if (ids[i] == nName)
            return &sHudPrompts[i];
    }

    return nullptr;
}

static constexpr int nObjectSearchDepth = 8;

// Re-run every update, not latched on first sight: the icon animates in, so a square derived from
// its first frame is far too small and the lock mask then pins it there for the widget's life.
static bool ComputeGlyphRect(uint8_t* pPromptArea, const HudPrompt& prompt, const SavedImage& saved, Rect& out)
{
    // The borrowed carrier is the ink blot, so its geometry means nothing. Measure from szAbove.
    auto refRect = ReadRect(GetState(prompt.szAbove ? FindNamedDeep(pPromptArea, NameId(prompt.szAbove)) : nullptr));
    if (refRect.Valid())
    {
        auto nRefHeight = refRect.nBottom - refRect.nTop;

        // Still at the start of the pop-in; the next update has something bigger to work from.
        auto nSide = nRefHeight * nGlyphScalePercent / 100;
        if (nSide < 1)
            return false;

        auto nGap = nRefHeight * nGlyphGapPercent / 100;
        auto nCentreX = (refRect.nLeft + refRect.nRight) / 2;

        out.nLeft = static_cast<int16_t>(nCentreX - nSide / 2);
        out.nRight = static_cast<int16_t>(out.nLeft + nSide);
        out.nBottom = static_cast<int16_t>(refRect.nTop - nGap);
        out.nTop = static_cast<int16_t>(out.nBottom - nSide);
        return true;
    }

    // No reference this frame. Transient, so keep an existing placement rather than falling back.
    if (saved.bPlaced)
        return false;

    // Nothing to hang it off. Provisional; the first frame with a reference overrides it.
    auto nWidth = saved.rect.nRight - saved.rect.nLeft;
    auto nHeight = saved.rect.nBottom - saved.rect.nTop;
    if (nWidth <= 0 || nHeight <= 0)
        return false;

    // Not std::min - Windows.h defines a min macro and NOMINMAX is not set here.
    auto nSide = nWidth < nHeight ? nWidth : nHeight;
    auto nCentreX = (saved.rect.nLeft + saved.rect.nRight) / 2;
    auto nCentreY = (saved.rect.nTop + saved.rect.nBottom) / 2;

    out.nLeft = static_cast<int16_t>(nCentreX - nSide / 2);
    out.nRight = static_cast<int16_t>(out.nLeft + nSide);
    out.nTop = static_cast<int16_t>(nCentreY - nSide / 2);
    out.nBottom = static_cast<int16_t>(out.nTop + nSide);
    return true;
}

static void ApplyHudGlyph(uint8_t* pHudRoot, uint8_t* pPromptArea, const HudPrompt& prompt)
{
    uint8_t* pImage = nullptr;
    auto bBorrowed = true;

    // First choice: the slot hud.mgb.desc names, which has its own geometry and backdrop. Searched
    // from the HUD root, since the art can put it beside the prompt's group rather than inside.
    uint8_t* pObject = nullptr;
    if (pHudRoot && prompt.szObject)
        pObject = GetSubArea(FindNamedDeep(pHudRoot, NameId(prompt.szObject), nObjectSearchDepth));

    if (pObject && prompt.szDesigned)
    {
        auto pDesigned = FindNamedDeep(pObject, NameId(prompt.szDesigned), nObjectSearchDepth);
        auto pDesignedImage = IsImage(pDesigned) ? pDesigned : FindImage(GetSubArea(pDesigned));

        // Only worth taking if it is not the carrier that would have been borrowed anyway.
        if (pDesignedImage && pDesignedImage != FindNamedDeep(pPromptArea, NameId(prompt.szSlot[0])))
        {
            pImage = pDesignedImage;
            bBorrowed = false;
        }
    }

    // Second choice, and the one stock PC data hits: build the widget as a sibling of szAbove.
    if (!pImage && prompt.szAbove)
    {
        uint8_t* pSiblingArea = nullptr;
        if (FindNamedDeepParent(pPromptArea, NameId(prompt.szAbove), &pSiblingArea))
        {
            static const auto nSlotName = NameId("i_jackalfix_glyph");

            // Still treated as borrowed: a fresh widget has no geometry, so it takes the same
            // placement path, and zeroing rotation and forcing opaque colours is a no-op on it.
            pImage = BuildImageChild(pSiblingArea, nSlotName);
        }
    }

    // Last resort if building fails: borrow the ink blot, the only spare Image.
    if (!pImage)
    {
        for (auto szSlot : prompt.szSlot)
        {
            if (!szSlot)
                break;

            auto pCandidate = FindNamedDeep(pPromptArea, NameId(szSlot));
            if (!pCandidate)
                continue;

            pImage = IsImage(pCandidate) ? pCandidate : FindImage(GetSubArea(pCandidate));
            if (pImage)
                break;
        }
    }

    if (!pImage)
        return;

    auto& saved = mSavedImages[pImage];

    if (ShouldShowGlyphs())
    {
        // Once per widget lifetime: the loader builds std::strings and walks the document list.
        if (!saved.bHeld)
        {
            auto nSprite = GetPadButtonImage ? GetPadButtonImage(prompt.nButton) : 0;
            if (nSprite == 0)
                return;

            auto pState = GetState(pImage);
            if (!pState)
                return;

            saved.nSprite = *reinterpret_cast<uintptr_t*>(pImage + nImageSprite);
            saved.nGlyph = nSprite;
            saved.rect = ReadRect(pState);
            saved.fRotation = *reinterpret_cast<float*>(pState + nStateRotation);
            saved.nPivotX = *reinterpret_cast<int16_t*>(pState + nStatePivotX);
            saved.nPivotY = *reinterpret_cast<int16_t*>(pState + nStatePivotY);
            saved.nLockMask = *reinterpret_cast<uint32_t*>(pImage + nWidgetLockMask);
            saved.bHeld = true;

            for (int i = 0; i < nColourCount; ++i)
                saved.nColours[i] = *reinterpret_cast<uint32_t*>(pState + nStateColour + i * 4);

            // A purpose built slot already has the right geometry and tint.
            if (!bBorrowed)
            {
                *reinterpret_cast<uintptr_t*>(pImage + nImageSprite) = saved.nGlyph;
                return;
            }

            // The ink blot is drawn at an angle, and a borrowed carrier brings that rotation.
            *reinterpret_cast<float*>(pState + nStateRotation) = 0.0f;
            *reinterpret_cast<int16_t*>(pState + nStatePivotX) = 0;
            *reinterpret_cast<int16_t*>(pState + nStatePivotY) = 0;

            for (int i = 0; i < nColourCount; ++i)
                *reinterpret_cast<uint32_t*>(pState + nStateColour + i * 4) = nOpaqueWhite;

            *reinterpret_cast<uint32_t*>(pImage + nWidgetLockMask) =
                saved.nLockMask | nRectLockBits | nRotationLockBits | nColourLockBits;
        }

        // bPlaced is not redundant with the comparison: switching to mouse and keyboard restores
        // the carrier's rect, so on the way back to the pad an identical rect must be rewritten.
        if (bBorrowed)
        {
            Rect placed;
            if (ComputeGlyphRect(pPromptArea, prompt, saved, placed)
                && (!saved.bPlaced || !placed.Same(saved.placed)))
            {
                WriteRect(GetState(pImage), placed);
                saved.placed = placed;
                saved.bPlaced = true;
            }
        }

        *reinterpret_cast<uintptr_t*>(pImage + nImageSprite) = saved.nGlyph;
    }
    else if (saved.bHeld)
    {
        if (auto pState = GetState(pImage))
        {
            WriteRect(pState, saved.rect);
            *reinterpret_cast<float*>(pState + nStateRotation) = saved.fRotation;
            *reinterpret_cast<int16_t*>(pState + nStatePivotX) = saved.nPivotX;
            *reinterpret_cast<int16_t*>(pState + nStatePivotY) = saved.nPivotY;

            for (int i = 0; i < nColourCount; ++i)
                *reinterpret_cast<uint32_t*>(pState + nStateColour + i * 4) = saved.nColours[i];
        }

        *reinterpret_cast<uintptr_t*>(pImage + nImageSprite) = saved.nSprite;
        *reinterpret_cast<uint32_t*>(pImage + nWidgetLockMask) = saved.nLockMask;
        saved.bHeld = false;

        // The engine owns the rect again, so the next placement starts from scratch.
        saved.bPlaced = false;
        saved.placed = Rect();
    }
}

// --------------------------------------------------------------------------------------------

class ButtonPrompts
{
public:
    ButtonPrompts()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            // FUN_105362E0, the 360 glyph loader. Anchored on the two null checks inside it; its
            // prologue is std::string boilerplate shared with unrelated functions.
            //
            //   3B F3        CMP  ESI,EBX          ; ESI = the resolved ui/360.mgb document
            //   74 3F        JZ   return 0
            //   ...
            //   3B C3        CMP  EAX,EBX          ; the sprite name
            //   74 2B        JZ   return 0
            static constexpr ptrdiff_t nGlyphLoaderEntry = 0x96;
            auto glyphPattern = dunia_pattern("3B F3 74 3F 8B 44 24 48 8B 0D ? ? ? ? 50 E8 ? ? ? ? 3B C3 74 2B 50 8D 4C 24 2C E8");
            if (!glyphPattern.empty())
            {
                auto pMatch = reinterpret_cast<uint8_t*>(glyphPattern.get_first());
                GetPadButtonImage = reinterpret_cast<GetPadButtonImage_t>(pMatch - nGlyphLoaderEntry);
            }

            // CHudPromptMgr::Update's per prompt loop. Hooking the call hands ECX over as the prompt.
            //
            //   8B FF           MOV  EDI,EDI          ; hot patch pad, and a useful anchor
            //   D9 44 24 10     FLD  dword [ESP+0x10] ; dt
            //   51              PUSH ECX
            //   8B 0E           MOV  ECX,[ESI]        ; the prompt array
            //   D9 1C 24        FSTP dword [ESP]
            //   03 CB           ADD  ECX,EBX          ; ECX = &prompts[i], stride 0x60
            //   E8 ? ? ? ?      CALL CHudPrompt::Update   <- hook
            auto hudPromptPattern = dunia_pattern("8B FF D9 44 24 10 51 8B 0E D9 1C 24 03 CB E8 ? ? ? ? 83 C7 01 83 C3 60 3B 7E 04 72 E4");
            if (!hudPromptPattern.empty() && GetPadButtonImage)
            {
                static auto HudPromptHook = safetyhook::create_mid(hudPromptPattern.get_first(nHudPromptUpdateCall), [](SafetyHookContext& regs)
                {
                    auto pPrompt = reinterpret_cast<uint8_t*>(regs.ecx);
                    if (!pPrompt)
                        return;

                    // Refcount zero means the prompt is not up and its group is hidden.
                    if (*reinterpret_cast<uint32_t*>(pPrompt + nHudPromptRefCount) == 0)
                        return;

                    auto pEntry = FindHudPrompt(*reinterpret_cast<uint32_t*>(pPrompt + nHudPromptName));
                    if (!pEntry)
                        return;

                    // ESI is the prompt vector inside CHud, so the HUD root comes free here.
                    auto pHud = reinterpret_cast<uint8_t*>(regs.esi) - nHudPromptVector;
                    auto pHudRoot = *reinterpret_cast<uint8_t**>(pHud + nHudRootArea);

                    auto pArea = *reinterpret_cast<uint8_t**>(pPrompt + nHudPromptArea);
                    if (pArea)
                        ApplyHudGlyph(pHudRoot, pArea, *pEntry);
                });
            }

            static auto ButtonPromptsCB = []()
            {
                bControllerPrompts = JackalFixSettings.GetInt(PREF_CONTROLLERPROMPTS) != 0;
            };

            ButtonPromptsCB();

            JackalFix::onIniFileChange() += []()
            {
                ButtonPromptsCB();
            };
        };
    }
} ButtonPrompts;
