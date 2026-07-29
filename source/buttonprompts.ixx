/*
  Restoring the in-game controller button prompts the PC build never asks for.

  Scope: the gameplay HUD prompts only. The menu nav bar is a second, entirely separate system; it
  is diagnosed below because the diagnosis was expensive and should not have to be repeated, but
  nothing here touches it and the menus are left exactly as the PC build ships them.

  Neither system was ever "disabled" - both were left intact in code and hollowed out in data.

  ---------------------------------------------------------------------------------------------
  THE CODE IS INTACT

  There is no platform check, no "pad connected" flag and no "show controller UI" toggle anywhere
  in the prompt path. Dunia never even works out whether a pad is plugged in: CInputDriverGamepad
  ::Poll calls XInputGetState and throws the return code away, ERROR_DEVICE_NOT_CONNECTED is never
  compared against anywhere in the image, and the only XInput imports are XInputGetState and
  XInputSetState. The glyph loader FUN_105362E0 is called unconditionally by all three of its
  callers, and it works: at runtime it resolves ui/360.mgb, resolves "360_a", and hands back a
  valid sprite. All of that happens on stock PC today.

  ---------------------------------------------------------------------------------------------
  MENUS: not implemented here, but here is why, so nobody re-derives it

  Two faults, both found and both fixable. The fix was written and tested, then removed because
  the menus are wanted as they ship. Notes only; no code below acts on any of this.

  The .mgb.desc files carry a <configuration> block the engine parses at page build time, and a
  nav bar prompt is authored like this, verbatim from the shipped PC hud.mgb.desc:

      <b_prompt1 text="Generic;OK" show="1"
                 icon_xenon="UI\360.mgb;360_a"
                 icon_ps3="UI\ps3.mgb;ps3_cross" />

  FUN_101D26D0 builds attribute names as <base> + a platform suffix, and the suffix constant is
  the literal "_pc". Across every .mgb.desc the game ships: 300 icon_xenon, 300 icon_ps3, 140
  show_pc - and zero icon_pc and zero bare icon. So the icon lookup misses on every prompt, every
  time. Pointing that one lookup at "_xenon" fixes it; show_pc has 140 real uses and has to be
  left alone.

  That alone changes nothing on screen, because of a second and larger problem. FUN_10189BA0 only
  pushes the sprite into the widget after finding a child named "i_placeholder", and that lookup
  returns null every time with every earlier stage healthy. Walking the child vector by hand
  explains it - a b_promptN button has exactly three children:

      "action"         crc 0x47CC8C92   magma::Placeholder   invisible, draws nothing
      "i_background"   crc 0x7A8A6532   magma::Image         the pill behind the label
      "t_button_text"  crc 0x49D78B9C   magma::Text          the label

  The PC art pass deleted the glyph Image, so the engine has been faithfully loading a 360 sprite
  into nowhere ever since. What survived is "action", a magma::Placeholder - it renders nothing
  but is still a Widget, so it still carries a state with a rect, and that is the console build's
  glyph anchor. In the shipped data its rect reads back empty, so a square off the pill's own
  height, 24x24, is the working fallback.

  The fix borrowed i_background as the carrier, hooked FUN_10189BA0 (CNavBarPrompt::SetIcon,
  attached flag at +0x51, element at +0x08) and swapped the suffix operand at rebuild time from
  CNavBarModule::SetLayout.

  ---------------------------------------------------------------------------------------------
  GAMEPLAY HUD: the container is there and nothing ever fills it

  Different mechanism entirely. hud.mgb.desc declares four button slots:

      <Interact_prompt   path="a_interact_object/a_interact_icon_anim/a_prompt_interact"   text="t_button" />
      <Inventory_prompt  path="a_inventory_object/a_inventory_icons_anim/a_prompt_interact" text="t_button" />
      <Reload_prompt     path="a_ammo_object/a_reload_icon_object/a_reload_icon_anims"      text="t_button" />
      <Swap_prompt       path="a_weapon_switch_object/a_weapon_switch/a_swap_icon"          text="t_button" />

  The strings Interact_prompt, Inventory_prompt, Reload_prompt, Swap_prompt and t_button do not
  exist anywhere in Dunia.dll. Those entries fall through a generic loop in CHud::BuildFromLayout
  that reads only the "path" attribute, resolves the widget, and appends it to a vector at
  HUD+0x338 whose sole consumer is a bulk hide. The text attribute is never read. So the slots are
  resolved once, hidden once, and forgotten - they render as empty containers.

  On console the same slots are filled by token substitution, {use} / {reload} / {heal} into
  t_button (xex 0x8296D130 and 0x82973D50). That code path does not exist in PC Dunia at all, and
  the a_prompt_interact container itself was deleted from the PC hud.mgb, so there is nothing to
  fill even if it did.

  The containers do become visible: FUN_1086B670 shows the parent group when a prompt fires and
  Area::SetVisible propagates into every child. So the group is on screen and empty, and all that
  is missing is content. This module builds a widget through magma's own factory and puts a glyph
  in it, which is why it composes with other mods instead of replacing the archive.

  Button assignment comes from the shipped console action map rather than being invented:
  use -> pad:a and pad:y, reload -> pad:x, tryuseied -> pad:right_trigger, heal ->
  pad:left_shoulder. Console interacts with Y, so that is what "use" resolves to here.

  ---------------------------------------------------------------------------------------------
  MAGMA, briefly, because every offset below depends on it

      Widget + 0x08          -> State
      Widget + 0x0C          -> component lock mask, bit set = engine must not write
      Widget + 0x3C          -> child Area          (AreaInstance and friends)
      Area   + 0x28 / + 0x2C -> child node vector, begin and end, stride 4
      node   + 0x08          -> zlib CRC-32 of the child's name; no strings are kept
      node   + 0x14          -> the drawable
      State  + 0x24/26/28/2A -> int16 left / right / top / bottom
      Image  + 0x20          -> sprite

  Type identity is exact vtable equality - magma::Image has no subclasses, its ObjectTypeInfo
  vtable has exactly one xref - so no engine calls are needed to walk or to write.

  A rect written into State+0x24 does not survive a frame on its own. Every tick the container
  walk evaluates the node's keyframes and RectState::blend writes the rect back from the .mgb
  unless the matching bit is set in Widget+0x0C. 0xF00 pins the rectangle, 0xE0 pins rotation and
  pivot, and 0x0F800000 pins the vertex colours. Everything else is left blending, so the prompts
  still fade in and out with the rest of the HUD.
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

// The four vertex colours, which is where a borrowed carrier's tint and alpha live. The ink blot
// is drawn faint, so a glyph inheriting its colours comes out barely visible.
static constexpr ptrdiff_t nStateColour = 0x44;
static constexpr int nColourCount = 4;
static constexpr uint32_t nOpaqueWhite = 0xFFFFFFFF;

// Component mask bits, from RotationState::blend and RectState::blend. Set means "do not blend".
// 5, 6, 7 are the rotation angle and its pivot; 8 to 11 are left, right, top and bottom;
// 23 to 27 are the vertex colours.
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
// Reimplemented rather than called, so nothing here needs an engine entry point.
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

// Runs a callback over an Area's direct children. Bounded, because a corrupt or unexpected
// object should cost nothing rather than walk off into the heap.
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

// The paths in hud.mgb.desc do not map onto the runtime tree one for one - the engine's own
// resolver descends through widget instances rather than child names, so a middle component like
// a_prompt_interact simply is not there to be found. Searching the subtree for the leaf is both
// simpler and more honest about what is actually known.
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

// Same walk, but hands back the Area the match was found in, which is what is needed to add a
// sibling next to it.
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

// Depth first, first match wins. Callers pass a subtree they have already narrowed down, so
// grabbing the first Image cannot wander into an unrelated icon.
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

    // Where the glyph currently sits. Placement is re-evaluated every update rather than latched
    // on the first one, so this is what the new result is compared against - see ComputeGlyphRect.
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
  On console the interact prompt is hand + ink blot + button, because the a_prompt_interact
  container and its t_button child exist in the console hud.mgb. The PC hud.mgb had that whole
  container deleted, so a_interact_icon holds two Images and three things to draw.

  The way out is not to edit the archive, it is to ask the engine for another widget. magma builds
  its tree through an ordinary factory, and every step of it is a normal callable function:

      Factory::CreateElementForType(ti)  0xABF0E0  makes the Element *and* its drawable, and -
                                                   this is the part that matters - hands the
                                                   drawable a properly constructed State. Calling
                                                   the raw Image factory instead leaves State as
                                                   uninitialised heap, which the first draw
                                                   dereferences.
      Factory::CreateKeyframe(ti)        0xABEE80
      Element::AddKeyframe(kf)           0xAB1C10
      Area::AddElement(e)                vtable +0x48, read at runtime
      Area::SetTime(0,0,0)               0xA973E0  forces one evaluation

  Ownership is single and clean, no refcounts: the Area destructor deletes its Elements, an
  Element deletes its drawable and its keyframes. So an Element built through the factory is freed
  exactly once, by the Area, on document unload. Allocating one with our own CRT instead would
  hand a foreign pointer to magma's pool free - hence the factory, never malloc.

  The keyframe is not optional and the reason is subtle. An Element with an empty keyframe vector
  is safe but invisible: the evaluator sets its keyframe index to 0xFFF meaning "none", and the
  draw gate refuses to draw anything with that index. One keyframe satisfies the gate. Its
  contents do not matter, because the rect, rotation and colour components are then pinned through
  the lock mask so nothing can blend over what is written.

  These are raw offsets rather than patterns, which is not this project's usual style. There is no
  choice: every one of them is an absolute the code only ever reaches through a global, and there
  is no instruction sequence to anchor on. They are all offsets from the image base, so a rebase
  is still handled.
*/

/*
  A backdrop behind the button was tried and dropped. The texture the console uses is not in the
  PC archive at all, so the attempt was to tint a white one that is - halftone_splatter or
  circle_glow out of ui/common.mgb - through the vertex colours. It reads as a smudge rather than
  the painted blot the console has, and looked worse than nothing.

  Recorded because the plumbing was the interesting part and works: a sprite in another document
  is two ordinary calls, FUN_105355B0 __thiscall(ECX = [0x11645C3C], std::string*) -> Document*
  and FUN_105361A0 __stdcall(Document*, const char*) -> Sprite*, and the engine string the first
  wants is only a header that can be built on the stack (+0x04 buffer, +0x14 size, +0x18 capacity;
  under 16 characters never allocates). If the real texture ever becomes available, that is how to
  reach it.
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

// One per Area, because the widget outlives the prompt and rebuilding it every frame would leak
// until the document unloaded.
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

// A dead end worth recording so nobody tries it twice: ui\textures\360\ ships a second set of
// glyphs, 360_a_shop next to 360_a and the same for b, x, y, lb and rb. They are not an in-world
// variant - they are flat monochrome outlines for the weapon shop, and using them over the world
// just gives a grey button. The single coloured set is the right one everywhere; the "orange"
// look of the console interact prompt is simply Y being yellow.

static constexpr ptrdiff_t nHudPromptName = 0x04;
static constexpr ptrdiff_t nHudPromptRefCount = 0x08;
static constexpr ptrdiff_t nHudPromptArea = 0x0C;
static constexpr ptrdiff_t nHudPromptUpdateCall = 14;

// CHud fields. The prompt vector is what the update loop holds, so the HUD itself is that minus
// its offset, and the widget root hangs off the HUD.
static constexpr ptrdiff_t nHudPromptVector = 0x244;
static constexpr ptrdiff_t nHudRootArea = 0x11C;

// Xbox button ids as FUN_10533F10 numbers them: A 0, B 1, X 2, Y 3, LT 4, RT 5, LB 6, RB 7.
static constexpr int32_t nPadX = 2;
static constexpr int32_t nPadY = 3;
static constexpr int32_t nPadRT = 5;
static constexpr int32_t nPadLB = 6;

// One HUD prompt: which button it should show, and where the glyph goes.
//
// szAbove is the icon the glyph sits above, and it is the important one - it names both the
// sibling the new widget is added next to and the rect everything is measured from. szSlot is the
// fallback carrier if the widget cannot be built, which costs the ink blot its own sprite.
//
// The buttons come from the shipped console action map - reload -> pad:x, tryuseied ->
// pad:right_trigger, heal -> pad:left_shoulder. The one the map cannot settle is "use", which is
// bound to pad:a and pad:y both; on console it is Y that interacts, so that is what these use.
struct HudPrompt
{
    const char* szName;
    int32_t nButton;
    const char* szSlot[2];  // carrier candidates, first one found wins
    const char* szAbove;    // sit the glyph above this sibling, if it is there
    const char* szObject;   // the prompt's container, searched from the HUD root
    const char* szDesigned; // the slot hud.mgb.desc names, if it turns out to exist
};

// The glyph is a square above the icon it belongs to, which is where the 360 build puts it, and
// it is sized from that icon rather than being a fixed number of pixels - the prompts are not all
// the same size, so a constant that suits the interact hand is wrong for everything else. These
// two are the knobs worth turning if it ever looks off.
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

/*
  Where the glyph goes, worked out from the icon it belongs above.

  Re-run on every update rather than latched the first time the prompt is seen, and that is not an
  optimisation left on the table - it is the fix for a real bug. The icon animates in: on the frame
  the prompt first appears its rect is still part way through the pop-in keyframes, so a square
  derived from it then is far too small, and because the rect is immediately pinned through the
  lock mask it stayed too small for the life of that widget. On a fresh boot the repair prompt came
  up tiny and only corrected itself once the widget was rebuilt at a moment the icon happened to be
  at full size.

  Recomputing means the glyph tracks the icon frame by frame, which is what the console build gets
  for free - there the button is a child of the same animation group. The cost is a shallow subtree
  walk per visible prompt, and the rect is only written when it has actually changed.
*/
static bool ComputeGlyphRect(uint8_t* pPromptArea, const HudPrompt& prompt, const SavedImage& saved, Rect& out)
{
    // The carrier is only borrowed - it is the ink blot backdrop, not a glyph slot, so its own
    // geometry means nothing here. The icon named by szAbove is the reference.
    auto refRect = ReadRect(GetState(prompt.szAbove ? FindNamedDeep(pPromptArea, NameId(prompt.szAbove)) : nullptr));
    if (refRect.Valid())
    {
        auto nRefHeight = refRect.nBottom - refRect.nTop;

        // Still at the very start of the pop-in. Nothing worth placing yet, and the next update
        // will have something bigger to work from.
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

    // No reference this frame. If the glyph has already been placed, keep it where it is - a
    // missing reference is a transient, and dropping the square back into the fallback position
    // every time the icon blinks out would be worse than doing nothing.
    if (saved.bPlaced)
        return false;

    // Nothing to hang it off at all, so just stop the square sprite being stretched across
    // whatever rectangle the carrier happens to have. Provisional: the first frame that does
    // produce a reference overrides it.
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

    // First choice: the slot hud.mgb.desc actually names. Searching from the prompt's own group
    // downwards would never see it if the art puts it beside that group rather than inside it, so
    // this starts at the HUD root and finds the prompt's container first. If it turns up, it is a
    // purpose built slot with its own geometry and its own backdrop, and nothing has to be
    // borrowed or built.
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

    // Second choice, and the one that actually applies on stock PC data: build the widget the
    // assets are missing, as a sibling of the icon the glyph belongs above. Costs nothing on
    // screen - the ink blot keeps its own job.
    if (!pImage && prompt.szAbove)
    {
        uint8_t* pSiblingArea = nullptr;
        if (FindNamedDeepParent(pPromptArea, NameId(prompt.szAbove), &pSiblingArea))
        {
            static const auto nSlotName = NameId("i_jackalfix_glyph");

            // Deliberately still treated as needing placement: a widget straight from the factory
            // has no geometry at all, so it goes through exactly the same positioning as a
            // borrowed carrier. Zeroing rotation and forcing opaque colours is a no-op on a fresh
            // state, which is the point - one code path, no special cases.
            pImage = BuildImageChild(pSiblingArea, nSlotName);
        }
    }

    // Last resort, if building fails: borrow the ink blot, which costs its sprite but is the only
    // Image going spare.
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
        // Asked for once per widget lifetime rather than once per frame - the loader builds
        // std::strings and walks the document list, which is not something to do at 60Hz.
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

            // A purpose built slot already has the right geometry and tint; only a borrowed or a
            // freshly built one needs correcting.
            if (!bBorrowed)
            {
                *reinterpret_cast<uintptr_t*>(pImage + nImageSprite) = saved.nGlyph;
                return;
            }

            // The ink blot is drawn at an angle for its hand-made look, and a borrowed carrier
            // brings that rotation with it.
            *reinterpret_cast<float*>(pState + nStateRotation) = 0.0f;
            *reinterpret_cast<int16_t*>(pState + nStatePivotX) = 0;
            *reinterpret_cast<int16_t*>(pState + nStatePivotY) = 0;

            // The blot is also drawn faint, and a glyph wearing its vertex colours is nearly
            // invisible.
            for (int i = 0; i < nColourCount; ++i)
                *reinterpret_cast<uint32_t*>(pState + nStateColour + i * 4) = nOpaqueWhite;

            *reinterpret_cast<uint32_t*>(pImage + nWidgetLockMask) =
                saved.nLockMask | nRectLockBits | nRotationLockBits | nColourLockBits;
        }

        // Placement, every update, so the glyph follows the icon through its pop-in instead of
        // being stuck at whatever size the first frame happened to show.
        //
        // The bPlaced test is not redundant with the comparison. Switching to mouse and keyboard
        // while a prompt is on screen hands the carrier's own rect back to the engine, so on the
        // way back to the pad the rect has to be written again even though the geometry works out
        // identical - which it does, because the icon has not moved. Comparing alone would see no
        // change and leave the glyph wearing the carrier's rectangle.
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

        // The engine owns the rect again, so the next time the pad comes back the placement is
        // worked out from scratch rather than compared against a stale one.
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
            // FUN_105362E0, the 360 glyph loader. The HUD has to ask for a sprite rather than
            // being handed one, so this is resolved rather than hooked. Anchored on the two null
            // checks inside it, because its prologue is std::string boilerplate shared with
            // unrelated functions.
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

            // CHudPromptMgr::Update's per prompt loop. Hooking the call rather than the callee
            // keeps the pattern short and hands over ECX already pointing at the prompt.
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

                    // Refcount zero means this prompt is not up, and its group is hidden. Acting
                    // on it would only fight the parse time hide.
                    if (*reinterpret_cast<uint32_t*>(pPrompt + nHudPromptRefCount) == 0)
                        return;

                    auto pEntry = FindHudPrompt(*reinterpret_cast<uint32_t*>(pPrompt + nHudPromptName));
                    if (!pEntry)
                        return;

                    // ESI is the prompt vector, which lives inside CHud, so the HUD and its
                    // widget root come for free without a second hook.
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
