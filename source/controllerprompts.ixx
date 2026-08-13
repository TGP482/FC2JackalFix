/*
  Restores the controller button prompts on the menu nav bar.

  Nothing is disabled in code. There is no platform check and no pad-connected flag; Dunia never
  works out whether a pad is present at all. FUN_105362E0 already resolves ui/360.mgb into a valid
  sprite on stock PC.

  The menus fail twice. FUN_101D26D0 builds attribute names with the literal "_pc" suffix, and
  across every shipped .mgb.desc there are 300 icon_xenon, 300 icon_ps3, 140 show_pc, zero icon_pc
  and zero bare icon, so the icon lookup misses on every prompt. Only the icon copy of the suffix
  is redirected. Do not touch show_pc, which has 140 real uses. Second, CNavBarPrompt::SetIcon
  (FUN_10189BA0, attached flag at +0x51, element at +0x08) pushes the sprite in only after finding
  a child named "i_placeholder", the glyph Image the PC art pass deleted. What survives is three
  other children:

      "action"         crc 0x47CC8C92   magma::Placeholder   invisible, draws nothing
      "i_background"   crc 0x7A8A6532   magma::Image         the pill behind the label
      "t_button_text"  crc 0x49D78B9C   magma::Text          the label

  "action" is the console build's glyph anchor. A Placeholder draws nothing but is still a Widget,
  so it carries a state with a rect; in shipped data that rect reads back empty, which is why the
  glyph is measured rather than anchored. Borrowing i_background as the carrier was tried and
  dropped: it hands the glyph the pill's rect, a wide rounded box rather than a square.

  Xbox 360 reaches the same result from the same data. Its config reader at xex 0x82480BF0 reads
  show, text and icon with a "_xenon" suffix and button with none, and CNavBarPrompt::SetIcon at
  0x82461698 finds "i_placeholder" the same way. Button id to sprite name is a 33 case jump table
  at 0x826802C0, byte indexed through an identity table at 0x8204D130; PC's FUN_10533F10 returns
  the same 33 names in the same order.

  There is no font path to port. magma registers a GlyphFont type (xex name 0x820FECE0), but its
  DynamicCast at 0x82E29A30 accepts EventHandler, AreaHandler and EngineObject and not Font, it
  carries no character lookup, and nothing in the image instantiates it. farcry2_25.mft stops at
  Latin-1 with no private use codepoints, and every icon_xenon in shipped data names a sprite.
  Prompts are sprites on both platforms.

  Size is the rect and nothing else. magma::Image has no scale field: Image::Draw, FUN_10AB93F0
  into FUN_10AB8CF0, builds the quad from State+0x24..0x2A unless ACTUALSIZE, bit 2 of the
  ImageState flags byte at +0x40, is set, and then the size comes off the texture and the rect
  only anchors it. A widget built through the factory comes out of a recycling pool carrying the
  last tenant's flags, which drew every glyph at 64x64 texels wherever it was placed. The UV pair
  at +0x30 to +0x3C rides along: tiling left at zero collapses a sprite to a single texel.

  PC draws each prompt as a clickable pill, i_background behind t_button_text. 360 has no pill at
  all, so the pill's visibility bit at node+0x34 goes off while a pad is active.

  The mouse pointer goes with them. magma keeps eight cursor slots on its screen manager, a
  registered bitmask at +0x28 and an enabled one at +0x29, and both are read at the head of the
  move handler FUN_10AB69F0, of the two button handlers FUN_10AB6690 and FUN_10AB6840, and again
  in the draw loop FUN_10AB59A0. Clearing the enabled bit takes the pointer off screen and makes
  it inert in the same move, and FUN_10AB5710, the position getter, already answers -32768 for a
  disabled slot. Nothing cheaper survives contact: SHOWCURSOR is read once when the document is
  set, by FUN_10108F40, and writes no live field; the cursor is drawn straight off the document's
  cursor stack through vtable +0x44 in FUN_10AA21A0 rather than through the node walk, so the
  visibility bit that hides an ordinary widget is never consulted; and parking the position off
  screen does not hold, since the next mouse event writes it back.

  Pad navigation does not go near any of this. FUN_104F0FB0 splits on a device type crc at the top
  and the gamepad arm reaches FUN_104EF990 and FUN_104EF340, which touch neither bitmask.

  The gameplay HUD is a different mechanism again, and console does not use sprites for it at all.
  Each prompt builds a token, L"{use}" or L"{reload}" or L"{heal}", hands it to magma's expander at
  xex 0x826AEDC0, and that walks the live action map and emits two character markup for every bound
  pad button: ~AA for a, ~XX for x, ~YY for y, ~LB for left shoulder. The expanded string goes to
  Text::SetText, and magma's text layout scans for a tilde, hashes the two characters after it and
  draws the matching glyph inline.

  All of that survived into Dunia.dll even though Far Cry 2's half of it did not. The scan is at
  0x1061B1F8, CMP AX,0x7E; the code map is at renderer+0x468 with its end at +0x478; the value at
  node+0x0C is a plain button id and the engine calls FUN_105362E0 on it itself. The map is filled
  by the constructor at 0x1061BF60, keyed by zlib CRC-32 of the two characters, ids in FUN_10533F10
  order: AA 0xA9601DBD -> 0, XX 0x560B1C65 -> 2, YY 0x38171DB2 -> 3, LB 0x85C7324A -> 6.

  It still cannot be used, and this was tried before the Image below was written. Console's layout
  binder reads both path and text off each prompt entry in hud.mgb.desc and resolves the named
  t_button child, while PC's CHud::BuildFromLayout reads path alone; Interact_prompt, Reload_prompt,
  Swap_prompt and t_button appear nowhere in Dunia.dll as strings. That much is only missing
  plumbing. The blocker is the data: dumping every prompt subtree at runtime finds no t_button and
  no magma::Text at all, so there is nothing to write markup into. A markup code that misses the
  map draws nothing rather than printing itself, so this failure and an unwritten string look
  identical on screen.

  Two console prompts turn out not to exist. There is no bed or sleep prompt in either binary:
  bedroll_readyToInteract is a GO state event and ActivateSleep an entity action, so a bed is an
  ordinary usable entity and shows the plain interact prompt. And the phone is not a hudprompt at
  all, just a CHud+0x1F4 member driven by a keyframe, with no call to the expander anywhere in its
  path; console draws the ringing phone with no glyph on it.

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
  rotation and pivot, 0x0F800000 the vertex colours.
*/

module;

#include <common.hxx>
#include <cstdint>
#include <set>

export module controllerprompts;

import common;
import dunia;
import inputdevice;

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

static constexpr ptrdiff_t nStateColour = 0x44;
static constexpr int nColourCount = 4;
static constexpr uint32_t nOpaqueWhite = 0xFFFFFFFF;

static constexpr ptrdiff_t nImageVtableRva = 0xEE6A04;
static constexpr ptrdiff_t nAreaInstanceVtableRva = 0xEE6BB4;
static constexpr ptrdiff_t nTextVtableRva = 0xEE63E4;
static constexpr ptrdiff_t nPlaceholderVtableRva = 0xEE9EA4;

// magma::ListBox and magma::RectShape, named by their own RTTI: 0xEE4794 carries
// ".?AVListBox@magma@@" and 0xEE796C ".?AVRectShape@magma@@". The shop page is built out of them,
// eight RectShapes for the green bars and two ListBoxes for the category and weapon lists, and
// with neither on this list the first pass at the arrows read no box off anything and gave up.
static constexpr ptrdiff_t nListBoxVtableRva = 0xEE4794;
static constexpr ptrdiff_t nRectShapeVtableRva = 0xEE796C;

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
// not a format limit.
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

// The same walk, handing over the Node as well. Visibility lives on the Node and the rect on the
// drawable behind it, so anything that has to hide what it measures needs both.
template <typename F>
static void ForEachChildNode(uint8_t* pArea, F&& fn)
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

        auto pDrawable = *reinterpret_cast<uint8_t**>(pNode + nNodeDrawable);
        if (pDrawable)
            fn(*reinterpret_cast<uint32_t*>(pNode + nNodeNameId), pNode, pDrawable);
    }
}

// Only these four carry a magma::State at +0x08. Reading a rect off anything else is a crash
// waiting: Button at 0xEE800C keeps something entirely different there, and the build that put
// it on this list came back with the same fault twice.
static bool HasRectState(uint8_t* pDrawable)
{
    if (!pDrawable)
        return false;

    auto nVtable = *reinterpret_cast<uintptr_t*>(pDrawable);

    return nVtable == Rva(nImageVtableRva) || nVtable == Rva(nAreaInstanceVtableRva)
        || nVtable == Rva(nTextVtableRva) || nVtable == Rva(nPlaceholderVtableRva)
        || nVtable == Rva(nListBoxVtableRva) || nVtable == Rva(nRectShapeVtableRva);
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

// ForEachChild hands over the drawable, and the visibility bit lives on the node in front of it.
static uint8_t* FindNamedNode(uint8_t* pArea, uint32_t nWanted)
{
    if (!pArea)
        return nullptr;

    auto ppBegin = *reinterpret_cast<uint8_t***>(pArea + nAreaChildBegin);
    auto ppEnd = *reinterpret_cast<uint8_t***>(pArea + nAreaChildEnd);
    if (!ppBegin || ppEnd < ppBegin)
        return nullptr;

    auto nCount = static_cast<size_t>(ppEnd - ppBegin);
    if (nCount > 64)
        return nullptr;

    for (size_t i = 0; i < nCount; ++i)
    {
        auto pNode = ppBegin[i];
        if (pNode && *reinterpret_cast<uint32_t*>(pNode + nNodeNameId) == nWanted)
            return pNode;
    }

    return nullptr;
}

// --------------------------------------------------------------------------------------------
// Building the widget the PC assets are missing
// --------------------------------------------------------------------------------------------

/*
  The PC prompt has no glyph child, so ask magma's factory for one instead of editing the archive:

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

// Not cached. The nav bar builds once per attach and its Areas go with the page, so a cache keyed
// on the Area pointer would hand back a freed widget the first time a new Area landed on a
// recycled address. Callers look for the child first and only build when it is absent.
static uint8_t* BuildImageChild(uint8_t* pArea, uint32_t nName)
{
    if (!pArea)
        return nullptr;

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

    return pDrawable;
}

// --------------------------------------------------------------------------------------------
// Menu nav bar
// --------------------------------------------------------------------------------------------

// FUN_105362E0. One stack argument, RET 4, so __stdcall.
using GetPadButtonImage_t = uintptr_t(__stdcall*)(int32_t);
static GetPadButtonImage_t GetPadButtonImage = nullptr;

// The suffix FUN_101D26D0's icon copy is pointed at, in place of "_pc".
static const char szXenonSuffix[] = "_xenon";

// FUN_10533F10's names in id order. Matching the icon value against these and going back through
// FUN_105362E0 keeps the glyph on one loader, and leaves the document manager out of it: the
// shipped values are written "UI\360.mgb;360_a", with a case and a separator no other caller of
// FUN_105355B0 uses.
static constexpr std::array<std::string_view, 33> sPadSpriteNames =
{{
    "360_a", "360_b", "360_x", "360_y", "360_lt", "360_rt", "360_lb", "360_rb",
    "360_ls_click", "360_rs_click", "360_back", "360_start",
    "360_ls", "360_ls_up", "360_ls_down", "360_ls_vertical", "360_ls_left", "360_ls_right",
    "360_ls_horizontal",
    "360_rs", "360_rs_up", "360_rs_down", "360_rs_vertical", "360_rs_left", "360_rs_right",
    "360_rs_horizontal",
    "360_pad", "360_pad_up", "360_pad_down", "360_pad_vertical", "360_pad_left", "360_pad_right",
    "360_pad_horizontal",
}};

// CNavBarPrompt, 0x54 bytes, constructed by FUN_10189E00 and held in a vector on the page's
// prompt set. +0x4C is the button id, 0x5C when the prompt was given an icon value instead.
static constexpr ptrdiff_t nPromptNode = 0x04;
static constexpr ptrdiff_t nPromptElement = 0x08;
static constexpr ptrdiff_t nPromptSprite = 0x40;
static constexpr ptrdiff_t nPromptButtonId = 0x4C;
static constexpr ptrdiff_t nPromptAttached = 0x51;
static constexpr size_t nPromptSize = 0x54;
static constexpr ptrdiff_t nPromptVtableRva = 0xE25054;

// Xbox button ids as FUN_10533F10 numbers them.
static constexpr int32_t nPadA = 0;
static constexpr int32_t nPadB = 1;
static constexpr int32_t nPadX = 2;
static constexpr int32_t nPadY = 3;
static constexpr int32_t nPadRT = 5;
static constexpr int32_t nPadLB = 6;
static constexpr int32_t nPadRB = 7;

// magma::Node. FUN_10AB13F0 is the engine's setter for bit 0 and FUN_10AB1A10 gates both drawing
// and child recursion on it, so clearing it takes the pill and everything under it off screen.
static constexpr ptrdiff_t nNodeFlags = 0x34;
static constexpr uint8_t nNodeVisibleBit = 0x01;

// magma::ImageState's tail, from the .mgb loader FUN_10AE1590 and the field copy FUN_10AD9400.
static constexpr ptrdiff_t nStateUvOffsetU = 0x30;
static constexpr ptrdiff_t nStateUvOffsetV = 0x34;
static constexpr ptrdiff_t nStateUvTilingU = 0x38;
static constexpr ptrdiff_t nStateUvTilingV = 0x3C;
static constexpr ptrdiff_t nStateImageFlags = 0x40;
static constexpr ptrdiff_t nStateShadowColour = 0x54;
static constexpr ptrdiff_t nStateShadowOffsetX = 0x58;
static constexpr ptrdiff_t nStateShadowOffsetY = 0x59;

// FUN_10AD9400 copies the whole +0x30 to +0x59 block back over the widget rather than lerping
// it, so pinning the rect and the colours is not enough to hold ACTUALSIZE down. Nothing else
// owns a widget we built, so every component is taken.
static constexpr uint32_t nAllComponentsLocked = 0xFFFFFFFF;

// magma::Text. Alignment is at +0x34, 0 left, 1 centre, 2 right, 3 justify.
static constexpr ptrdiff_t nTextString = 0x18;
static constexpr ptrdiff_t nTextAlignX = 0x34;
static constexpr int32_t nAlignCentre = 1;
static constexpr int32_t nAlignRight = 2;

// MSVC's std::string and std::wstring. The 16 byte buffer doubles as the pointer once capacity
// reaches the element count that fills it.
static constexpr ptrdiff_t nStringBuffer = 0x04;
static constexpr ptrdiff_t nStringSize = 0x14;
static constexpr ptrdiff_t nStringCapacity = 0x18;
static constexpr uint32_t nStringLocalCapacity = 16;
static constexpr uint32_t nWideLocalCapacity = 8;

// A .mgb.desc attribute value. Anything longer is a bad read, not a long value.
static constexpr uint32_t nStringSizeLimit = 0x400;

// Console draws the glyph 30x30 at 720p. Rect units are not pixels, so the square is calibrated
// rather than derived: a glyph sized at 6/5 of the label's point size counted 11 pixels across in
// a 1280x720 capture, and 30 over 11 of that is the square wanted, so 36 over 11 of the point
// size. All 33 glyph textures are 64x64, so one square suits every button.
static constexpr ptrdiff_t nTextStatePointSize = 0x40;
static constexpr int nMenuGlyphSizeNumerator = 36;
static constexpr int nMenuGlyphSizeDenominator = 11;
static constexpr int nMenuGlyphPixels = 30;

// The one length in rect units whose size on screen is known, so everything else is measured
// against it. The HUD has no label to derive a point size from and borrows this instead; it is
// sound because magma is one flat unit space, Area::Draw pushing a translate and nothing else.
static int32_t nCalibratedGlyphSide = 0;

// If the point size cannot be read. 150 units was what the counted build fell back to, so it
// takes the same 30 over 11 and lands on the same square whichever branch runs.
static constexpr int nMenuGlyphSideFallback = 409;

// Beside the label. Counted between the glyph and the nearest letter on a 1280x720 capture of the
// console display options: 7 pixels on "(B) Back", 7 on "(X) Default", 10 on "Apply (Y)" and 9 on
// "Accept (A)". The tighter end is the one to take: the wider two are the labels whose nearest
// letter is a "y", which stops short of the glyph on its own.
//
// Held against the disc rather than against the unit space, so it is 6 pixels only where it was
// counted and stays the same share of the glyph everywhere else. Both are lengths across, so the
// scale divides out and the ratio survives any resolution or aspect. Fixing it in units instead
// leaves it growing with the display's width while the glyph grows with its height, which reads
// loose on an ultrawide.
static constexpr int nMenuGlyphGapPixels = 6;
static constexpr int nMenuGlyphRoundPixels = 30;

// The quad is not the glyph, so the gap cannot be counted against the side. The sprite carries
// its own transparent margin, and the rect sizes the quad the sprite draws into rather than the
// disc inside it. The gap the player sees starts at the disc, a margin's worth of nothing further
// in than where the rect ends.
//
// Solved rather than counted, since neither edge of the margin is visible to count. Three builds
// of this file ask for the gap in three different lengths, 0.4, 0.2 and 4/7 of the side, and
// their captures read 43, 54 and 30 pixels between the "(B) Back" glyph and its label. Two
// unknowns against three readings, and the quad comes out 65.0, 65.0 and 64.9 pixels across with
// the disc at 34.0, 34.0 and 34.3, which is the 34 the disc measures on the capture directly.
//
// The disc is the fraction of the quad, not a length, so both hold at any size or aspect: the
// margin is half of what the quad has over the disc, taken off whatever the quad works out to.
static constexpr int nMenuGlyphQuadPixels = 65;
static constexpr int nMenuGlyphDiscPixels = 34;

// FUN_10AB4F50. Anchored past the SEH prologue, on the two null checks that reach the font
// through Text+0x58, since the prologue itself is shared with every other guarded function.
//
//   8B F9        MOV  EDI,ECX
//   8B 74 24 54  MOV  ESI,[ESP+0x54]   ; the State argument
//   85 F6        TEST ESI,ESI
//   75 03        JNZ  +3
//   8B 77 08     MOV  ESI,[EDI+0x08]   ; null means the widget's own state
//   8B 47 58     MOV  EAX,[EDI+0x58]   ; the font holder
using GetTextAlignOffset_t = int32_t(__thiscall*)(uint8_t*, const wchar_t*, void*);
static GetTextAlignOffset_t GetTextAlignOffset = nullptr;

static std::string_view StringView(const uint8_t* pString)
{
    if (!pString)
        return {};

    auto nSize = *reinterpret_cast<const uint32_t*>(pString + nStringSize);
    auto nCapacity = *reinterpret_cast<const uint32_t*>(pString + nStringCapacity);
    if (nSize == 0 || nSize > nStringSizeLimit)
        return {};

    auto pData = nCapacity < nStringLocalCapacity
        ? reinterpret_cast<const char*>(pString + nStringBuffer)
        : *reinterpret_cast<const char* const*>(pString + nStringBuffer);

    return pData ? std::string_view(pData, nSize) : std::string_view();
}

// "UI\360.mgb;360_a" -> 0. The document half is dropped; every shipped value names 360.mgb, and
// the ps3_ names the ps3 suffix would produce have no PC sprite to reach anyway.
static int32_t PadButtonFromIconValue(std::string_view sValue)
{
    auto nSeparator = sValue.rfind(';');
    if (nSeparator == std::string_view::npos)
        return -1;

    auto sName = sValue.substr(nSeparator + 1);

    for (size_t i = 0; i < sPadSpriteNames.size(); ++i)
    {
        if (sName == sPadSpriteNames[i])
            return static_cast<int32_t>(i);
    }

    return -1;
}

static void SetNodeVisible(uint8_t* pNode, bool bVisible)
{
    if (!pNode)
        return;

    auto& nFlags = *reinterpret_cast<uint8_t*>(pNode + nNodeFlags);
    nFlags = bVisible ? (nFlags | nNodeVisibleBit) : (nFlags & ~nNodeVisibleBit);
}

static void ResetImageState(uint8_t* pState)
{
    *reinterpret_cast<float*>(pState + nStateRotation) = 0.0f;
    *reinterpret_cast<int16_t*>(pState + nStatePivotX) = 0;
    *reinterpret_cast<int16_t*>(pState + nStatePivotY) = 0;

    *reinterpret_cast<float*>(pState + nStateUvOffsetU) = 0.0f;
    *reinterpret_cast<float*>(pState + nStateUvOffsetV) = 0.0f;
    *reinterpret_cast<float*>(pState + nStateUvTilingU) = 1.0f;
    *reinterpret_cast<float*>(pState + nStateUvTilingV) = 1.0f;

    // Clears ACTUALSIZE along with FLIPHORIZONTAL and FLIPVERTICAL.
    *reinterpret_cast<uint8_t*>(pState + nStateImageFlags) = 0;

    *reinterpret_cast<uint32_t*>(pState + nStateShadowColour) = 0;
    *reinterpret_cast<int8_t*>(pState + nStateShadowOffsetX) = 0;
    *reinterpret_cast<int8_t*>(pState + nStateShadowOffsetY) = 0;

    for (int i = 0; i < nColourCount; ++i)
        *reinterpret_cast<uint32_t*>(pState + nStateColour + i * 4) = nOpaqueWhite;
}

// FUN_10AB4F50 returns where the string starts inside the widget's rect, which for right
// alignment is the box width less the text width. Forcing the alignment for the one call is the
// measurement; the field is a plain member, so nothing blends the original back in the meantime.
static int32_t MeasureTextWidth(uint8_t* pText, int32_t nBoxWidth)
{
    if (!GetTextAlignOffset || !pText || nBoxWidth <= 0)
        return -1;

    auto pString = pText + nTextString;
    auto nCapacity = *reinterpret_cast<uint32_t*>(pString + nStringCapacity);
    auto pWide = nCapacity < nWideLocalCapacity
        ? reinterpret_cast<const wchar_t*>(pString + nStringBuffer)
        : *reinterpret_cast<const wchar_t* const*>(pString + nStringBuffer);

    if (!pWide)
        return -1;

    auto& nAlign = *reinterpret_cast<int32_t*>(pText + nTextAlignX);
    auto nSaved = nAlign;

    nAlign = nAlignRight;
    auto nOffset = GetTextAlignOffset(pText, pWide, nullptr);
    nAlign = nSaved;

    auto nWidth = nBoxWidth - nOffset;
    return (nWidth >= 0 && nWidth <= nBoxWidth) ? nWidth : -1;
}

// FUN_10AC0AF0's three cases. Justify lands on the left branch there too.
static int32_t TextLeftInBox(int32_t nAlign, int32_t nBoxWidth, int32_t nTextWidth)
{
    if (nAlign == nAlignCentre)
        return (nBoxWidth - nTextWidth) / 2;

    if (nAlign == nAlignRight)
        return nBoxWidth - nTextWidth;

    return 0;
}

/*
  Console puts the glyph outboard, and the button picks the side rather than the slot.

  Its quit box reads "(B) Cancel" against "Accept (A)" with B on b_prompt2, while its display
  options reads "Accept (A)" and "Apply (Y)" behind their labels with "(X) Default" and "(B) Back"
  in front, on b_prompt3 and b_prompt4. A and Y sit behind in both, B and X in front, so b_prompt2
  lands on either side depending on what it carries.

  The slots decide it only where there is no button: b_prompt1 is the rightmost and b_prompt4 the
  leftmost, and the node the prompt hangs off carries the slot's name crc. +0x4C reads 0x5C on
  those, which no button id matches.

  Comparing element rects across the prompt set was the first attempt and gets a lone prompt
  wrong: the main menu shows only "Accept (A)", and with nothing to span, a midpoint puts it in
  front of the label where console puts it behind.
*/
static bool GlyphSitsRight(uint8_t* pPrompt)
{
    switch (*reinterpret_cast<int32_t*>(pPrompt + nPromptButtonId))
    {
    case nPadA:
    case nPadY:
        return true;

    case nPadB:
    case nPadX:
        return false;

    default:
        break;
    }

    auto pNode = *reinterpret_cast<uint8_t**>(pPrompt + nPromptNode);
    if (!pNode)
        return false;

    static const auto nFirstSlot = NameId("b_prompt1");
    static const auto nSecondSlot = NameId("b_prompt2");

    auto nName = *reinterpret_cast<uint32_t*>(pNode + nNodeNameId);
    return nName == nFirstSlot || nName == nSecondSlot;
}

// Split out of the placement so it runs on either device. Inside ComputeMenuGlyphRect it sat past
// the keyboard branch of ApplyMenuGlyph, so a launch reaching a computer without the pad having
// dressed one menu prompt left the side at zero and the shop fell back to the constant, which is
// the menu's square and not the computer's.
static int32_t CalibrateGlyphSide(uint8_t* pArea)
{
    static const auto nLabelName = NameId("t_button_text");

    auto pTextState = GetState(FindNamedDeep(pArea, nLabelName, 0));
    if (!pTextState)
        return nCalibratedGlyphSide;

    auto fPointSize = *reinterpret_cast<float*>(pTextState + nTextStatePointSize);
    if (fPointSize < 1.0f || fPointSize > 4096.0f)
        return nCalibratedGlyphSide;

    auto nSide = static_cast<int32_t>(fPointSize * nMenuGlyphSizeNumerator / nMenuGlyphSizeDenominator);
    if (nSide > 0)
        nCalibratedGlyphSide = nSide;

    return nCalibratedGlyphSide;
}

static bool ComputeMenuGlyphRect(uint8_t* pArea, bool bRightSide, Rect& out)
{
    static const auto nLabelName = NameId("t_button_text");

    auto pText = FindNamedDeep(pArea, nLabelName, 0);
    auto pTextState = GetState(pText);
    auto box = ReadRect(pTextState);
    if (!box.Valid())
        return false;

    auto nSide = CalibrateGlyphSide(pArea);

    // Without a point size to read there is nothing to calibrate against, so the constant stands
    // in and is recorded, exactly as the counted build did.
    if (nSide < 1)
    {
        nSide = nMenuGlyphSideFallback;
        nCalibratedGlyphSide = nSide;
    }

    // Taking the margin here leaves the gap measured to the disc.
    auto nDisc = nSide * nMenuGlyphDiscPixels / nMenuGlyphQuadPixels;
    auto nMargin = (nSide - nDisc) / 2;
    auto nGap = nDisc * nMenuGlyphGapPixels / nMenuGlyphRoundPixels - nMargin;

    auto nCentreY = (box.nTop + box.nBottom) / 2;

    out.nTop = static_cast<int16_t>(nCentreY - nSide / 2);
    out.nBottom = static_cast<int16_t>(out.nTop + nSide);

    // The measurement is the branch that runs. The three captures the margin was solved from all
    // sit against the text and not against the label rect, which is only true of this branch, so
    // the box edge below is a fallback and nothing more.
    //
    // An empty label measures zero rather than failing, and zero is the one width that must not
    // be taken: a centred label of no width puts both edges on the box's midpoint, which is where
    // the text will be, so the glyph lands on top of it. The box edge is wrong by the padding but
    // it is wrong outboard, where a glyph waiting for its label belongs.
    auto nBoxWidth = box.nRight - box.nLeft;
    auto nTextWidth = MeasureTextWidth(pText, nBoxWidth);
    auto bMeasured = nTextWidth > 0;

    auto nAlign = *reinterpret_cast<int32_t*>(pText + nTextAlignX);
    auto nTextLeft = bMeasured ? box.nLeft + TextLeftInBox(nAlign, nBoxWidth, nTextWidth) : box.nLeft;
    auto nTextRight = bMeasured ? nTextLeft + nTextWidth : box.nRight;

    out.nLeft = static_cast<int16_t>(bRightSide ? nTextRight + nGap : nTextLeft - nGap - nSide);
    out.nRight = static_cast<int16_t>(out.nLeft + nSide);
    return true;
}

// Runs at the SetIcon call in CNavBarPrompt::OnAttach, where the widget tree is live and the
// sprite the config read cached is in hand, and again for every attached prompt when the active
// device flips.
// All defined below, past this. This function precedes every one of them.
static bool IsReadable(const void* pAddress);
static bool IsObject(uint8_t* pObject);
static bool ComputeBazaarGlyphRect(uint8_t* pLabel, Rect& out);
static void TintBazaarGlyph(uint8_t* pState);
static const char* ShopLetterFromButton(int32_t nButton);
static uintptr_t GetShopSprite(const char* szLetter);

/*
  Whether the computer's own message boxes wear its glyphs. On when the computer's page opens, off
  when it closes, with F10 left in as a manual override.

  Deciding it from the prompt does not work. A bazaar button resolves from anywhere, so asking
  whether one exists says yes in every menu. A flag set from CBazaarComputerUI's destructor never
  clears, the object outliving its page. Climbing owners from the prompt for a page named after
  the bazaar finds no such name: the chain runs 0xEE6524, 0xEE4A1C, 0xEE48CC, 0xEE4464 and stops,
  carrying name crcs F3874570 and E5EC7051 at the top two.

  The page carries it. The menu page base vtable at 0xF4747C takes enter at slot 2 and leave at
  slot 3: FUN_10CDB480 builds the page's document, hangs it on +0x160 and sets the shown byte,
  FUN_10CDAE90 clears that byte and hands the document back. CBazaarComputerUI's vtable at
  0xE9DA7C overrides both, FUN_10734630 for enter and FUN_10731EA0 for leave. Reading a state off
  the object instead will not work: the page pointers at +0x180 and +0x184 are written by
  FUN_107330C0 and never written back to zero anywhere in the image.
*/
static constexpr int nShopGlyphToggleKey = VK_F10;
static bool bShopGlyphsOnPrompts = false;

static void ApplyMenuGlyph(uint8_t* pPrompt)
{
    // IsLivePrompt vouches for the prompt, not for what it points at. A prompt can be attached
    // and still hold a stale element: a crash came back with 0x21 in hand, faulting on 0x5D,
    // which is that plus the child area offset, read straight through a null check.
    auto pElement = *reinterpret_cast<uint8_t**>(pPrompt + nPromptElement);
    if (!IsObject(pElement))
        return;

    auto pArea = *reinterpret_cast<uint8_t**>(pElement + nWidgetChildArea);
    if (!IsReadable(pArea))
        return;

    // Ahead of the device branch below, which returns before reaching the placement.
    CalibrateGlyphSide(pArea);

    // Not "i_placeholder". Under that name FUN_10189BA0 would find the slot on the next attach
    // and push prompt+0x40 straight back into it, undoing the blank on the way to keyboard.
    static const auto nGlyphName = NameId("i_jackalfix_glyph");
    static const auto nBackgroundName = NameId("i_background");

    auto pImage = FindNamedDeep(pArea, nGlyphName, 0);
    auto pBackground = FindNamedNode(pArea, nBackgroundName);

    auto nSprite = *reinterpret_cast<uintptr_t*>(pPrompt + nPromptSprite);

    auto bOnComputer = bShopGlyphsOnPrompts;

    if (bOnComputer)
    {
        if (auto szLetter = ShopLetterFromButton(*reinterpret_cast<int32_t*>(pPrompt + nPromptButtonId)))
        {
            if (auto nShopSprite = GetShopSprite(szLetter))
                nSprite = nShopSprite;
        }
    }

    if (!IsPadActiveDevice() || nSprite == 0)
    {
        if (pImage)
            *reinterpret_cast<uintptr_t*>(pImage + nImageSprite) = 0;

        // Every prompt that draws at all ships with its pill visible, so this is the stock state.
        SetNodeVisible(pBackground, true);
        return;
    }

    if (!pImage)
        pImage = BuildImageChild(pArea, nGlyphName);

    auto pState = GetState(pImage);
    if (!pState)
        return;

    // A prompt on the computer's page is a computer prompt in every respect. The menu's square
    // comes off the label's point size and the menu's own scale, and both are wrong there: the
    // computer's font is larger and its page draws at twice the scale, so a message box A came out
    // twice the size of the buttons below it and on the wrong side. Placement and colour go
    // through the shop's instead.
    static const auto nLabelName = NameId("t_button_text");

    Rect rect;
    auto bPlaced = false;

    if (bOnComputer)
    {
        if (auto pLabel = FindNamedDeep(pArea, nLabelName, 0))
            bPlaced = ComputeBazaarGlyphRect(pLabel, rect);
    }

    if (!bPlaced && !ComputeMenuGlyphRect(pArea, GlyphSitsRight(pPrompt), rect))
        return;

    ResetImageState(pState);
    WriteRect(pState, rect);

    if (bPlaced)
        TintBazaarGlyph(pState);

    *reinterpret_cast<uint32_t*>(pImage + nWidgetLockMask) = nAllComponentsLocked;
    *reinterpret_cast<uintptr_t*>(pImage + nImageSprite) = nSprite;

    SetNodeVisible(pBackground, false);
}

// --------------------------------------------------------------------------------------------
// Catching up mid page
// --------------------------------------------------------------------------------------------

/*
  Nothing drives the nav bar per frame. CNavBarModule only touches a prompt when the page is
  attached, so a row already on screen when the player puts the pad down would otherwise keep the
  glyphs until they navigated somewhere. Prompts attached since the last page change are kept and
  walked again on the device change event.

  The pointers can go stale: a page teardown frees the prompt vector, and PromptSet::FindOrCreate
  grows it, which moves every prompt without detaching anything. So each one is checked rather
  than trusted. CNavBarPrompt opens with a vftable, which tells a live object from a freed block,
  and OnDetach (FUN_10189CF0) clears the attached flag at +0x51, which rules out one that is
  allocated but off screen.
*/

static std::set<uint8_t*> sAttachedPrompts;

// VirtualQuery rather than a structured exception handler, so the check costs the same whether
// the page is there or not and the walk stays an ordinary function.
static bool IsReadable(const void* pAddress)
{
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(pAddress, &mbi, sizeof(mbi)) != sizeof(mbi))
        return false;

    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0)
        return false;

    auto nEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return reinterpret_cast<uintptr_t>(pAddress) + nPromptSize <= nEnd;
}

// Comfortably past Dunia's mapped size, so a vtable check is a range test and not an exact one.
static constexpr uintptr_t nDuniaImageSize = 0x2000000;

/*
  Readable is not the same as real. IsReadable says a pointer is mapped, and a crash came back
  faulting on 0x5D with 0x21 in hand, which is 0x21 plus the 0x3C child area offset: something
  small and not a pointer arrived where an element should have been and was read straight through.

  An object has a vtable, and every vtable in this tree lives in Dunia. Checking that the first
  dword points into the module costs one read and rejects a small integer outright.
*/
static bool IsObject(uint8_t* pObject)
{
    if (!pObject || !IsReadable(pObject))
        return false;

    auto nVtable = *reinterpret_cast<uintptr_t*>(pObject);
    auto nBase = reinterpret_cast<uintptr_t>(hDunia);

    return nVtable >= nBase && nVtable < nBase + nDuniaImageSize && IsReadable(reinterpret_cast<void*>(nVtable));
}


static bool IsLivePrompt(uint8_t* pPrompt)
{
    if (!pPrompt || !IsReadable(pPrompt))
        return false;

    if (*reinterpret_cast<uintptr_t*>(pPrompt) != Rva(nPromptVtableRva))
        return false;

    return *(pPrompt + nPromptAttached) != 0;
}

static void RefreshMenuGlyphs()
{
    for (auto it = sAttachedPrompts.begin(); it != sAttachedPrompts.end(); )
    {
        if (!IsLivePrompt(*it))
        {
            it = sAttachedPrompts.erase(it);
            continue;
        }

        ApplyMenuGlyph(*it);
        ++it;
    }
}

// --------------------------------------------------------------------------------------------
// Gameplay HUD
// --------------------------------------------------------------------------------------------

/*
  Console's mechanism does not port. It sets a t_button Text under each prompt container from an
  expanded {use} or {reload} token, but the PC art pass took those containers out. A dump of every
  prompt subtree at runtime found a_prompt_interact 01808B52 absent, t_button 1361D4C3 absent, and
  across interact, switchweapon and the inventory family not one magma::Text anywhere: every
  drawable is a Placeholder at vtable rva EE9EA4, an AreaInstance at EE6BB4 or an Image at EE6A04.
  So the glyph is built as an Image, the way the nav bar's is.

  The same dump gave the anchors to hang it off, by crc:

      interact       a_interact_icon 7AD39F66 -> stain CB2D676F, i_use_icon FC09AC1D
      switchweapon   a_weapon_switch 00E1C03A -> a_swap_icon 0509A605 -> i_stain 7395FC18,
                     i_arrow 608F7549
      inventory      a_inventory_icons 7A3A3A37 -> stain, i_watch E8B3D151, i_wrench A9264364,
                     i_phone FCF70CAA, i_ied D43D37D9

  Prompt+0x0C is the Area itself, not an element wrapping one; the child search runs straight off
  it. Buttons come from the shipped console action map: use -> pad:a and pad:y, reload -> pad:x,
  tryuseied -> pad:right_trigger, heal -> pad:left_shoulder. Console interacts with Y.
*/

// CHudPrompt, stride 0x60 on the manager's array. +0x08 is the show request, +0x0C the Area the
// <Prompt path="..."> entry resolved to at layout build time.
static constexpr ptrdiff_t nHudPromptName = 0x04;
static constexpr ptrdiff_t nHudPromptShown = 0x08;
static constexpr ptrdiff_t nHudPromptArea = 0x0C;

// The call inside CHudPromptMgr::Update's per prompt loop, which hands the prompt over as ECX.
static constexpr ptrdiff_t nHudPromptUpdateCall = 0x0E;

// Square, in 720p pixels, converted through the menu glyph's calibrated side. Until a menu prompt
// has been placed there is nothing to convert with, so the fallback is the older behaviour: a
// share of the icon the glyph sits over. The gap stays relative to that icon either way.
static constexpr int nHudGlyphGapPercent = 20;
static constexpr int nHudGlyphFallbackPercent = 85;

/*
  Dead end: centring the glyph on the screen rather than on the icon beneath it.

  Console does centre on the screen. Counted on 1280x720 captures, Xenia puts the interact glyph
  across x 628..651 and the weapon swap glyph across 624..653, both centred on 639 give or take
  the antialiasing, while the icons underneath are not symmetric about the screen at all: the swap
  arrow's own pixels centre on 653. Centring on the anchor rect, which is what the code below
  does, therefore lands 2 pixels right on interact and 3 on the swap. Both attempts to close that
  gap failed and it is left open deliberately; two pixels are not worth a fragile placement.

  The first attempt derived screen centre from the glyph calibration. Its own sanity check
  rejected it every frame, so it never once ran.

  The second read the canvas instead of guessing it. That much is solid and is worth keeping
  written down. The canvas is not a constant anywhere in the binary; magma refills it from the
  active screen's extent every frame in FUN_10AB59A0. CInputHandler's mouse event builder
  FUN_104EFAD0 is where reading it is plainest, clamping a freshly moved cursor to it:

      MOV   ECX,[0x10FD4588]          ; owner of the per thread table
      MOV   EDI,[0x10F98AB8]          ; which slot of it
      CALL  0x1002F880                ; the table
      MOV   EAX,[EAX + EDI*4 + 0xC]   ; the render context
      MOVZX EAX,word ptr [EAX + 0x34] ; canvas width
      ...
      MOVZX ECX,word ptr [ECX + 0x36] ; canvas height

  with the other end of both clamps a literal zero, so the canvas origin is the top left corner.
  Half that width still did not move the glyph, which points at the rects in this subtree being
  local to a parent rather than absolute on the canvas. Nothing here can see a parent: a node
  holds its children and no back pointer. FUN_10A97020 looks like the way through, since it is
  what the cursor is hit tested against and the cursor is in canvas units, but that was not
  followed up.
*/

// Position and size do not convert at the same rate in this tree, which no single affine
// transform explains and which is not understood. The square is right: at nPixels 30 the interact
// glyph measures 26 across and at 34 the swap glyph measures 30, a ratio of 1.15 against the 1.13
// asked for, and both sit within a pixel or two of Xenia's 23 and 29. The offset is not. Two
// captures whose anchors are pixel identical to the build before them put a requested 17 pixel
// drop at 33 and a requested 12 at 23: a straight line of slope two.
//
// So the offset rate is measured rather than modelled. Every distance that positions the glyph
// halves; the square does not.
static constexpr int nHudOffsetHalving = 2;

// The HUD area draws larger than the nav bar's. Counted on a 1280x720 capture: the menu's side
// measured 30 pixels where the same side measured 34 here.
static constexpr int nHudGlyphMeasured = 34;

// szAbove is both the sibling the glyph is added next to and the rect it is measured from.
// nPixels is the square and nDown a downward nudge on top of the gap, both in 720p pixels.
//
// The two nudges are counted off the Xenia captures. Interact: console leaves 43 pixels between
// the glyph and the hand, the port left 57. Weapon swap: 39 against 52. Rounded out of the
// antialiasing on both bounds that is 17 down and 12 down. The inventory prompts have no console
// capture to count against, so they take interact's number as the closer of the two.
struct HudPrompt
{
    const char* szName;
    int32_t nButton;
    const char* szAbove;
    int nPixels;
    int nDown;
};

static constexpr std::array<HudPrompt, 8> sHudPrompts =
{{
    { "interact",     nPadY,  "i_use_icon", 30, 17 },
    { "switchweapon", nPadY,  "i_arrow",    34, 12 },
    { "watch",        nPadY,  "i_watch",    30, 17 },
    { "ratchet",      nPadY,  "i_wrench",   30, 17 },
    { "map",          nPadY,  "i_watch",    30, 17 },
    { "ied",          nPadRT, "i_ied",      30, 17 },
    { "syringe",      nPadLB, "i_watch",    30, 17 },
    { "reload",       nPadX,  nullptr,      30, 17 },
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

// The glyph has to be a sibling of the icon it sits over, so the search reports the Area it found
// the icon in rather than just the icon.
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

// Re-run every update rather than latched on first sight: the icon animates in, so a square taken
// from its first frame is far too small and the lock mask then pins it there for good.
static bool ComputeHudGlyphRect(uint8_t* pAnchor, const HudPrompt& prompt, Rect& out)
{
    auto anchor = ReadRect(GetState(pAnchor));
    if (!anchor.Valid())
        return false;

    auto nHeight = anchor.nBottom - anchor.nTop;

    auto nSide = nCalibratedGlyphSide > 0
        ? nCalibratedGlyphSide * prompt.nPixels / nHudGlyphMeasured
        : nHeight * nHudGlyphFallbackPercent / 100;

    if (nSide < 1)
        return false;

    auto nGap = nHeight * nHudGlyphGapPercent / 100;

    auto nCentreX = (anchor.nLeft + anchor.nRight) / 2;

    auto nDrop = nCalibratedGlyphSide > 0
        ? prompt.nDown * nCalibratedGlyphSide / (nMenuGlyphPixels * nHudOffsetHalving)
        : 0;

    out.nLeft = static_cast<int16_t>(nCentreX - nSide / 2);
    out.nRight = static_cast<int16_t>(out.nLeft + nSide);
    out.nBottom = static_cast<int16_t>(anchor.nTop - nGap + nDrop);
    out.nTop = static_cast<int16_t>(out.nBottom - nSide);
    return true;
}

static void ApplyHudGlyph(uint8_t* pPrompt, const HudPrompt& prompt)
{
    if (!GetPadButtonImage || !prompt.szAbove)
        return;

    auto pArea = *reinterpret_cast<uint8_t**>(pPrompt + nHudPromptArea);
    if (!pArea || !IsReadable(pArea))
        return;

    static const auto nGlyphName = NameId("i_jackalfix_glyph");

    uint8_t* pSiblingArea = nullptr;
    auto pAnchor = FindNamedDeepParent(pArea, NameId(prompt.szAbove), &pSiblingArea);
    if (!pAnchor || !pSiblingArea)
        return;

    auto pImage = FindNamedDeep(pSiblingArea, nGlyphName, 0);

    if (!IsPadActiveDevice())
    {
        if (pImage)
            *reinterpret_cast<uintptr_t*>(pImage + nImageSprite) = 0;

        return;
    }

    auto nSprite = GetPadButtonImage(prompt.nButton);
    if (nSprite == 0)
        return;

    if (!pImage)
        pImage = BuildImageChild(pSiblingArea, nGlyphName);

    auto pState = GetState(pImage);
    if (!pState)
        return;

    Rect rect;
    if (!ComputeHudGlyphRect(pAnchor, prompt, rect))
        return;

    ResetImageState(pState);
    WriteRect(pState, rect);

    *reinterpret_cast<uint32_t*>(pImage + nWidgetLockMask) = nAllComponentsLocked;
    *reinterpret_cast<uintptr_t*>(pImage + nImageSprite) = nSprite;
}

// --------------------------------------------------------------------------------------------
// The mouse pointer
// --------------------------------------------------------------------------------------------

/*
  Only the enabled bitmask is touched. Far Cry 2's own FUN_104EEB20 looks like the obvious call
  and is not: past disabling the slots it zeroes the active cursor index at manager+0x68,
  unregisters every cursor through FUN_10AB5FA0, which drops its input capture record, and clears
  a byte at +0x05 on each entry of the device record array at manager+0x00. That last one takes
  the pad down with it. Calling it cost pad navigation and pad vibration, both at once.

  FUN_10AB75C0(screenManager, slot, enabled) is the narrow lever underneath it: one bit of
  screenManager+0x29, with the leave event FUN_10AB6D00 fired first so nothing stays highlighted.
  Registration, the active index and the record flags are left alone.

  The screen manager is reached the way FUN_104EEB20 reaches it, through a global rather than
  through anything handed to a hook:

      MOV ECX,[0x10FE3178]
      ADD ECX,0x4
*/

static constexpr ptrdiff_t nScreenManagerRva = 0xFE3178;
static constexpr ptrdiff_t nScreenManagerOffset = 0x04;
static constexpr ptrdiff_t nCursorEnabledMask = 0x29;
static constexpr int nCursorSlotCount = 8;

using SetCursorEnabled_t = void(__thiscall*)(void*, uint32_t, uint32_t);
static SetCursorEnabled_t SetCursorEnabled = nullptr;

static uint8_t nSavedCursorMask = 0;
static bool bInCursorUpdate = false;

static uint8_t* ScreenManager()
{
    auto pGlobal = *reinterpret_cast<uint8_t**>(Rva(nScreenManagerRva));
    return pGlobal ? pGlobal + nScreenManagerOffset : nullptr;
}

static void DisableCursors()
{
    auto pManager = ScreenManager();
    if (!pManager || !SetCursorEnabled)
        return;

    auto nEnabled = *(pManager + nCursorEnabledMask);
    if (nEnabled == 0)
        return;

    nSavedCursorMask = nEnabled;

    for (int i = 0; i < nCursorSlotCount; ++i)
    {
        if (nEnabled & (1 << i))
            SetCursorEnabled(pManager, i, 0);
    }
}

static void RestoreCursors()
{
    auto pManager = ScreenManager();
    if (!pManager || !SetCursorEnabled || nSavedCursorMask == 0)
        return;

    for (int i = 0; i < nCursorSlotCount; ++i)
    {
        if (nSavedCursorMask & (1 << i))
            SetCursorEnabled(pManager, i, 1);
    }

    nSavedCursorMask = 0;
}

// --------------------------------------------------------------------------------------------

// --------------------------------------------------------------------------------------------
// The shop computer
// --------------------------------------------------------------------------------------------

/*
  Console draws these in the computer's own green outline style, and the art for it ships on PC.
  It is not in ui/weapon_bazaar.mgb: that package names three sprites between the xex and Dunia,
  check_computer, lock and pc_menu_rollover, and no button among them. It is in ui/360.mgb, the
  package the menu and HUD glyphs already come from, whose dependency list carries a second set
  alongside the gold one:

      360_a.xbt   360_b.xbt   360_x.xbt   360_y.xbt   360_lb.xbt   360_rb.xbt
      360_a_shop  360_b_shop  360_x_shop  360_y_shop  360_lb_shop  360_rb_shop

  The _shop suffix is the computer's green outline set, sitting unused on PC because the page that
  referenced it was replaced. Sprite names inside the package follow the texture names, so the
  glyph the menu asks for as button_a is button_a_shop here.

  FUN_105362E0 is that lookup: a document, a name, and three calls.

      CALL FUN_105355B0        ; ui/360.mgb            <- document cached off this return
      CALL FUN_10533F10        ; button id -> name
      CALL FUN_100BD1D0        ; name object from a char*
      CALL FUN_10A99300        ; document resolves it
      CALL FUN_100BCF90        ; name object away again

  Only the name changes, so the document is taken from the engine's own return rather than asked
  for. Asking meant handing FUN_105355B0 a std::string, and a hand built one crashed inside the
  game's msvcr80. Nothing is constructed now: the glyph loader runs constantly for the menu and
  the HUD, and its document is cached on the way past.

  Placement, counted off a 360 capture: B Cancel, A Add/Remove, Y Checkout, with the glyph to the
  left of the text bar rather than inside it. The bar stays. Hiding crc 0xC03AFD13 to make room
  takes the label with it: that crc is the whole button visual, not the pill.

  The buttons are asked for by name rather than reached from CBazaarComputerUI's cached label
  pointers. Those pointers are not the widgets on screen: searching every area reachable from them
  finds the label for Checkout and for none of the other four. FUN_10731100 shows the lookup in
  two calls:

      PUSH "WEAPON_BAZAAR_BUTTON_CANCEL"
      PUSH EAX                  ; a buffer for the name object
      CALL FUN_10AA7150         ; cdecl, returns the object
      PUSH EAX
      MOV  ECX,[0x11645C3C]     ; the ui manager
      CALL FUN_10730610         ; returns the element's drawable

  Both calls sit at fixed offsets inside FUN_10731100, which is already pattern matched for the
  hook. The lookup resolves against whichever page is up, but the checkout page does not reuse the
  shop page's names for two of its three buttons; see sBazaarButtons.

  The label inside a button is crc 0x81FDCEB5, from the capture.
*/

static constexpr size_t nSpriteNameSize = 64;

// Offsets into FUN_105362E0 from the pattern this module already anchors on, to the three calls
// that turn a name into a sprite. Each is an E8 rel32.
static constexpr ptrdiff_t nMakeSpriteNameCall = 0x1D;
static constexpr ptrdiff_t nResolveSpriteCall = 0x29;
static constexpr ptrdiff_t nFreeSpriteNameCall = 0x34;

// Just past the CALL FUN_105355B0 inside the glyph loader, where EAX is the ui/360.mgb document.
// Behind the pattern this module anchors on rather than ahead of it.
static constexpr ptrdiff_t nUiDocumentLoaded = -0x3A;

// Entry of CBazaarComputerUI::RefreshFocus to the MOV EDI,ECX, so the UI is in EDI.
static constexpr ptrdiff_t nBazaarRefreshThis = 0x09;

// The two calls inside it that turn an element name into a drawable.
static constexpr ptrdiff_t nMakeElementNameCall = 0x55;
static constexpr ptrdiff_t nFindElementCall = 0x60;
static constexpr ptrdiff_t nUiManagerRva = 0x1645C3C;

static constexpr uint32_t nBazaarLabelId = 0x81FDCEB5;


/*
  The computer's green, written out rather than borrowed.

  Borrowing fails three ways. A label's colour dims with focus, so an unfocused button draws a
  nearly black glyph. The cart on Checkout reads FF000000, its green being in the texture. Scaling
  a borrowed value moves the hue, red and blue swapping between the two candidate byte orders.

  Byte order, measured: writing 0xFF87FFD0 rendered a yellow green of about 149,200,98. In memory
  that dword is D0 FF 87 FF, and reading those bytes as red, green, blue, alpha gives 208,255,135,
  whose ratios match. The dword is 0xAABBGGRR, alpha highest and red lowest.

  The value is the console glyph's hue at full brightness. Sampling gave 4CB381, and writing that
  made the glyph vanish: a capture carries the dimming the page put on it and a tint multiplies,
  so feeding it back dims twice. What survives sampling is the channel ratio, 0.30 to 0.70 to
  0.51. Scaled until green reaches full that is 6CFFB7, which in this order is FFB7FF6C. The
  87FFD0 picked off the bar first was the highlight, not the glyph.
*/
static constexpr uint32_t nBazaarGlyphColour = 0xFFB7FF6C;

// Counted on a 1280x720 capture: the calibrated square written whole came out 63 pixels across.
static constexpr int nBazaarGlyphPixels = 30;
static constexpr int nBazaarGlyphMeasured = 63;

// The glyph's own count off the same page, taken after the UI scaling was corrected: 65 units
// drew 26 pixels where 30 are wanted. The arrows keep the 63 above, which is their own count.
static constexpr int nBazaarGlyphSideMeasured = 55;

// Counted off Xenia: the glyph clears the bar's left edge by two pixels.
static constexpr int nBazaarGapPixels = 2;

using MakeElementName_t = void*(__cdecl*)(void*, const char*);
using FindElement_t = uint8_t*(__thiscall*)(void*, void*);
using MakeSpriteName_t = void(__thiscall*)(void*, const char*);
using ResolveSprite_t = uintptr_t(__thiscall*)(void*, void*);
using FreeSpriteName_t = void(__thiscall*)(void*);

// FUN_10720880, which does the name and the lookup in one and hands back the Element rather than
// what it draws.
using FindElementByName_t = uint8_t*(__stdcall*)(const char*);

static MakeElementName_t MakeElementName = nullptr;
static FindElement_t FindElement = nullptr;
static FindElementByName_t FindElementByName = nullptr;
static MakeSpriteName_t MakeSpriteName = nullptr;
static ResolveSprite_t ResolveSprite = nullptr;
static FreeSpriteName_t FreeSpriteName = nullptr;
static void* pUiDocument = nullptr;

struct BazaarButton
{
    const char* szElement;
    const char* szLetter;
    int32_t nFallbackButton;
};

// The sprite name is inferred from the texture name, 360_a_shop.xbt and friends, so more than one
// spelling is tried and the gold sprite sits behind them all. button_y_shop alone resolved to
// nothing on the last build, which is what the ordering is for.
static constexpr std::array<const char*, 2> sShopSuffixes = {{ "_shop", "" }};
static constexpr std::array<const char*, 2> sShopPrefixes = {{ "button_", "360_" }};

/*
  The checkout page does not name its buttons the way the shop page does. Only its cancel has a
  WEAPON_BAZAAR_BUTTON_ spelling.

  FUN_107330C0 has the rest. It resolves six in a row into the UI from +0x1A8: BAZAAR_BUTTON_
  CANCEL, ADDREMOVE and CHECKOUT, then CANCEL_CHECKOUT, ADDREMOVE_CHECKOUT and CHECKOUT_CHECKOUT
  at +0x1B4 through +0x1BC. The last two have no WEAPON_ twin.
*/
static constexpr std::array<BazaarButton, 6> sBazaarButtons =
{{
    { "WEAPON_BAZAAR_BUTTON_CANCEL",          "b", nPadB },
    { "WEAPON_BAZAAR_BUTTON_ADDREMOVE",       "a", nPadA },
    { "WEAPON_BAZAAR_BUTTON_CHECKOUT",        "y", nPadY },
    { "WEAPON_BAZAAR_BUTTON_CANCEL_CHECKOUT", "b", nPadB },
    { "BAZAAR_BUTTON_ADDREMOVE_CHECKOUT",     "a", nPadA },
    { "BAZAAR_BUTTON_CHECKOUT_CHECKOUT",      "y", nPadY },
}};

// The page holds plenty that is never drawn, and a template left at its defaults reads back a box
// like 0..0 by -24236..4334. Anything outside the canvas by an order of magnitude is one of those
// and not a thing on screen, so nothing is measured against it.
static constexpr int nBazaarSaneBound = 4000;

static bool TryReadRect(uint8_t* pDrawable, Rect& out)
{
    if (!HasRectState(pDrawable))
        return false;

    auto pState = GetState(pDrawable);
    if (!pState || !IsReadable(pState))
        return false;

    auto box = ReadRect(pState);
    if (!box.Valid())
        return false;

    if (box.nLeft < -nBazaarSaneBound || box.nRight > nBazaarSaneBound
        || box.nTop < -nBazaarSaneBound || box.nBottom > nBazaarSaneBound)
        return false;

    out = box;
    return true;
}

// A child vector that reads like one. +0x28 and +0x2C on a class that is not an Area are whatever
// happens to sit there, so the pair has to be checked before either is walked.
static bool LooksLikeArea(uint8_t* pObject)
{
    if (!IsObject(pObject))
        return false;

    auto ppBegin = *reinterpret_cast<uint8_t***>(pObject + nAreaChildBegin);
    auto ppEnd = *reinterpret_cast<uint8_t***>(pObject + nAreaChildEnd);
    if (!ppBegin || !IsReadable(ppBegin) || ppEnd < ppBegin)
        return false;

    return static_cast<size_t>(ppEnd - ppBegin) <= 64;
}

// Most widgets hang an Area off +0x3C. magma::Button at 0xEE800C does not: it is one. Its vtable
// takes Area::Draw at +0x44, which walks the node vector at +0x28 on the object itself, and
// Area::AddElement at +0x48, which pushes onto that same vector. Reading and building elements
// works on the Button unchanged.
static uint8_t* AsChildArea(uint8_t* pDrawable)
{
    if (!pDrawable)
        return nullptr;

    auto pArea = *reinterpret_cast<uint8_t**>(pDrawable + nWidgetChildArea);
    if (LooksLikeArea(pArea))
        return pArea;

    return LooksLikeArea(pDrawable) ? pDrawable : nullptr;
}

// Matching the drawable itself rather than a name, because the id a Node carries is the name of
// the slot it fills and not the name the element was asked for.
static bool FindDrawableParent(uint8_t* pArea, uint8_t* pWanted, uint8_t** ppParent, int nMaxDepth = 6,
    int nDepth = 0)
{
    if (!pArea || !pWanted || nDepth > nMaxDepth)
        return false;

    auto bFound = false;

    ForEachChildNode(pArea, [&](uint32_t, uint8_t*, uint8_t* pDrawable)
    {
        if (bFound)
            return;

        if (pDrawable == pWanted)
        {
            bFound = true;
            if (ppParent)
                *ppParent = pArea;
        }
        else if (auto pSub = AsChildArea(pDrawable))
        {
            if (pSub != pArea)
                bFound = FindDrawableParent(pSub, pWanted, ppParent, nMaxDepth, nDepth + 1);
        }
    });

    return bFound;
}

/*
  FUN_10730610 checks twice. Having found the element and checked its class it checks the drawable
  behind it against a second type at 0xEE7E90, and returns nothing when that misses. The checkout
  page's add/remove and checkout fail there: the names resolve, the drawables are the wrong class.

  FUN_107330C0 does not use it for those. It goes through FUN_10720880, the same lookup by way of
  FUN_10181390 without the second check, taking the drawable off the Element afterwards. That is
  the fallback here, tried only where the strict lookup found nothing.
*/
static uint8_t* FindBazaarElement(const char* szElement)
{
    if (MakeElementName && FindElement)
    {
        auto pManager = *reinterpret_cast<void**>(Rva(nUiManagerRva));

        if (pManager && IsReadable(pManager))
        {
            uint8_t nameObject[nSpriteNameSize]{};

            if (auto pName = MakeElementName(nameObject, szElement))
            {
                auto pDrawable = FindElement(pManager, pName);
                if (IsObject(pDrawable))
                    return pDrawable;
            }
        }
    }

    if (!FindElementByName)
        return nullptr;

    auto pElement = FindElementByName(szElement);
    if (!IsObject(pElement))
        return nullptr;

    auto pDrawable = *reinterpret_cast<uint8_t**>(pElement + nElementDrawable);
    return IsObject(pDrawable) ? pDrawable : nullptr;
}

/*
  The Area a drawable sits in, for names that answer with something other than a button.

  BAZAAR_BUTTON_ADDREMOVE_CHECKOUT and BAZAAR_BUTTON_CHECKOUT_CHECKOUT resolve to objects holding
  nothing: no Area at +0x3C and no child vector of their own. Each is a part of its button. The
  Area that holds it is the button's, and carries both the label to measure from and the glyph.
*/
static uint8_t* FindHoldingArea(uint8_t* pDrawable)
{
    static constexpr std::array<const char*, 2> sPages =
    {{
        "BAZAAR_SHOP_PAGE",
        "BAZAAR_CHECKOUT_PAGE",
    }};

    for (auto szPage : sPages)
    {
        uint8_t* pParent = nullptr;

        if (FindDrawableParent(AsChildArea(FindBazaarElement(szPage)), pDrawable, &pParent))
            return pParent;
    }

    return nullptr;
}

static uintptr_t CallTarget(uint8_t* pCall)
{
    return reinterpret_cast<uintptr_t>(pCall) + 5 + *reinterpret_cast<int32_t*>(pCall + 1);
}

static uintptr_t ResolveNamedSprite(const char* szName)
{
    if (!pUiDocument || !MakeSpriteName || !ResolveSprite || !FreeSpriteName)
        return 0;

    // One object, built from the string and handed to the document, exactly as FUN_105362E0 does.
    uint8_t nameObject[nSpriteNameSize]{};

    MakeSpriteName(nameObject, szName);
    auto nSprite = ResolveSprite(pUiDocument, nameObject);
    FreeSpriteName(nameObject);

    return nSprite;
}

// The name that last resolved, so a gold button_b standing in for a missing button_b_shop is
// visible in the log rather than looking identical to it.
static uintptr_t GetShopSprite(const char* szLetter)
{
    for (auto szSuffix : sShopSuffixes)
    {
        for (auto szPrefix : sShopPrefixes)
        {
            std::string sName = szPrefix;
            sName += szLetter;
            sName += szSuffix;

            if (auto nSprite = ResolveNamedSprite(sName.c_str()))
                return nSprite;
        }
    }

    return 0;
}

// Left of the bar, not inside it, so the box edge is what the glyph sits against rather than the
// measured text the menu uses.
// The _shop art is a white outline; the green is the tint. All four corners, so no gradient.
static void TintBazaarGlyph(uint8_t* pState)
{
    for (int i = 0; i < nColourCount; ++i)
        *reinterpret_cast<uint32_t*>(pState + nStateColour + i * 4) = nBazaarGlyphColour;
}

static bool ComputeBazaarGlyphRect(uint8_t* pLabel, Rect& out)
{
    auto pTextState = GetState(pLabel);
    auto box = ReadRect(pTextState);
    if (!box.Valid())
        return false;

    /*
      The menu's square is not this page's square. Writing the calibrated 65 units here, which is
      30 pixels across in the menu, measured 63 pixels across on the computer: the bazaar page
      draws at a little over twice the menu's scale. Deriving it from the label's point size, the
      way the nav bar does, was worse again, since the computer's font is the larger of the two.

      So the calibration is carried across by the ratio that was counted, 30 pixels wanted against
      the 63 that 65 units produced. That keeps it following the resolution the way the calibrated
      value does, without pretending the two pages share a scale.
    */
    auto nBase = nCalibratedGlyphSide > 0 ? nCalibratedGlyphSide : nMenuGlyphSideFallback;

    auto nSide = nBase * nBazaarGlyphPixels / nBazaarGlyphSideMeasured;
    if (nSide < 1)
        return false;

    /*
      Clear of the bar's left edge, by the two pixels counted off Xenia.

      A negative left is fine. An earlier read of a capture took the glyph sitting at -41..-11 for
      something clipped away by the area's edge, and moved it inside the bar to compensate. It was
      not clipped, it was dim: the tint it had been given was the label's, and an unfocused label
      is nearly black. The area does not clip, and outside is where console has it.
    */
    /*
      Measured from the button's own left edge, which is zero, rather than from the label box.
      The label does not start at the same place on every button: Checkout's box begins at 33
      because the cart sprite has the first 33, so hanging the glyph off the box put Checkout's
      inside its bar and on top of the cart while the others sat outside. The bar is the button,
      so the button's edge is the one console measures from.
    */
    auto nGap = nBase * nBazaarGapPixels / nBazaarGlyphSideMeasured;
    auto nCentreY = (box.nTop + box.nBottom) / 2;

    out.nTop = static_cast<int16_t>(nCentreY - nSide / 2);
    out.nBottom = static_cast<int16_t>(out.nTop + nSide);
    out.nLeft = static_cast<int16_t>(-(nGap + nSide));
    out.nRight = static_cast<int16_t>(out.nLeft + nSide);
    return true;
}

// Temporary, alongside the placement rather than instead of it. One line per button the first
// time it is tried, naming the step that stopped it. Four things have to line up and a blank
// screen says which of them failed only if it is asked.
// Button ids as FUN_10533F10 numbers them, to the letter the sprite names use.
static const char* ShopLetterFromButton(int32_t nButton)
{
    switch (nButton)
    {
    case nPadA: return "a";
    case nPadB: return "b";
    case nPadX: return "x";
    case nPadY: return "y";
    default: return nullptr;
    }
}


static void ApplyBazaarGlyph(const BazaarButton& button)
{
    auto pButton = FindBazaarElement(button.szElement);

    auto pArea = AsChildArea(pButton);
    if (!pArea)
        pArea = FindHoldingArea(pButton);

    if (!pArea)
        return;

    auto pLabel = FindNamedDeep(pArea, nBazaarLabelId, 0);
    if (!pLabel || !IsReadable(pLabel))
        return;

    static const auto nGlyphName = NameId("i_jackalfix_glyph");

    auto pImage = FindNamedDeep(pArea, nGlyphName, 0);

    // Nothing of the game's is hidden either way: the bar stays on both devices, console keeps it.
    if (!IsPadActiveDevice())
    {
        if (pImage)
            *reinterpret_cast<uintptr_t*>(pImage + nImageSprite) = 0;

        return;
    }

    auto nSprite = GetShopSprite(button.szLetter);
    if (nSprite == 0 && GetPadButtonImage)
        nSprite = GetPadButtonImage(button.nFallbackButton);

    if (nSprite == 0)
        return;

    if (!pImage)
        pImage = BuildImageChild(pArea, nGlyphName);

    auto pState = GetState(pImage);
    if (!pState)
        return;

    Rect rect;
    if (!ComputeBazaarGlyphRect(pLabel, rect))
        return;

    ResetImageState(pState);
    WriteRect(pState, rect);

    TintBazaarGlyph(pState);

    *reinterpret_cast<uint32_t*>(pImage + nWidgetLockMask) = nAllComponentsLocked;
    *reinterpret_cast<uintptr_t*>(pImage + nImageSprite) = nSprite;
}

/*
  The category arrows, the "<<" and ">>" either side of the WEAPONS bar.

  Nothing to port. The xex names exactly what Dunia names around them, BAZAAR_CATEGORY_LIST and
  nothing on either side of it, so the LB and RB in the 360 capture are drawn by its own copy of
  ui/weapon_bazaar.mgb and by no code at all. The PC package puts a bordered box with "<<" and
  ">>" in the same two places instead.

  They are not in the page tree. A walk of BAZAAR_SHOP_PAGE six levels deep, through every class
  holding an Area, accounts for the whole page: the category bar at 0..335 by 0..25, the diamond
  and total groups, the list frame at 780 by 455, the weapon rows, the three buttons. No arrows,
  no page title, none of the slider's scroll arrows. They are members of the list rather than
  children of anything, and the slider keeps its own the same way.
*/

/*
  The pill does not fill its texture, so the square is worked back from a measurement rather than
  set to the size wanted. A 30 unit square drew it 19 across and 18 down in a 1280x720 capture,
  against 28 by 26 for the round a/b/y glyph in the same square; a second at 58 by 33 measured 34
  by 20. It covers a little under three fifths either way, so console's 37 by 20 wants 63 by 33,
  leaving 13 units of empty texture each side of the pill.
*/
static constexpr int nBazaarArrowQuadWide = 63;
static constexpr int nBazaarArrowQuadHigh = 33;
static constexpr int nBazaarArrowWidePixels = 37;

static constexpr int nBazaarArrowInsetPixels =
    (nBazaarArrowQuadWide - nBazaarArrowWidePixels) / 2;

// Measured against a Xenia capture. On the box's inner edge the pill cleared the bar by 5 on the
// left and 6 on the right against console's 9, and sat 2 pixels high of it, the "<<" box the
// anchor comes from not sitting quite on the bar's middle.
static constexpr int nBazaarArrowNudgePixels = 4;
static constexpr int nBazaarArrowDropPixels = 2;

/*
  magma::ListBox, read off its own Draw at 0x10A9F590, and why the glyph is measured from the
  arrow rather than from the bar.

  The two arrows hang on the object at +0x58 and +0x74, each drawn under a translate of its own
  taken from a position pair at +0xB0/+0xB4 and +0xC0/+0xC4 less an origin on the list's State.
  The list writes that pair itself, out of the width of whatever the arrow holds, and butts the
  arrow's right edge against the bar's left edge. Stock the arrow is 50 across; widened to 128 by
  a glyph reaching to -78 it moved 74 further left, up beside the page title.

  Anchoring on the bar returns every unit of reach as an equal shift the other way. The arrow's
  own box does not move underneath it, so the pill takes the inner edge of that box with the empty
  13 going over the bar.
*/
static constexpr ptrdiff_t nListBoxFirstArrow = 0x58;
static constexpr ptrdiff_t nListBoxSecondArrow = 0x74;

struct BazaarArrow
{
    const char* szLetter;
    const char* szGlyphName;
    int32_t nFallbackButton;
    ptrdiff_t nMember;
    bool bLeftOfBar;
};

static constexpr std::array<BazaarArrow, 2> sBazaarArrows =
{{
    { "lb", "i_jackalfix_lb", nPadLB, nListBoxFirstArrow, true },
    { "rb", "i_jackalfix_rb", nPadRB, nListBoxSecondArrow, false },
}};

// Every child except the glyph we put there. Console draws no box at all where the arrows are,
// only the pill, so the "<<" and its border come off with the pad in hand and go back without it.
static void ShowArrowContent(uint8_t* pArea, uint32_t nGlyphName, bool bVisible)
{
    ForEachChildNode(pArea, [&](uint32_t nId, uint8_t* pNode, uint8_t*)
    {
        if (nId != nGlyphName)
            SetNodeVisible(pNode, bVisible);
    });
}

// What the arrow covers in its own space, which is the union of whatever it was built from: the
// bordered box and the "<<" or ">>" across it. Its own state rect is no use, a Button carrying
// nothing at +0x08 to read.
static bool ArrowContentBox(uint8_t* pArea, uint32_t nGlyphName, Rect& out)
{
    auto bAny = false;

    ForEachChildNode(pArea, [&](uint32_t nId, uint8_t*, uint8_t* pDrawable)
    {
        if (nId == nGlyphName)
            return;

        Rect box;
        if (!TryReadRect(pDrawable, box))
            return;

        if (!bAny)
        {
            out = box;
            bAny = true;
            return;
        }

        out.nLeft = out.nLeft < box.nLeft ? out.nLeft : box.nLeft;
        out.nTop = out.nTop < box.nTop ? out.nTop : box.nTop;
        out.nRight = out.nRight > box.nRight ? out.nRight : box.nRight;
        out.nBottom = out.nBottom > box.nBottom ? out.nBottom : box.nBottom;
    });

    return bAny;
}

static void ApplyBazaarArrow(const BazaarArrow& arrow, uint8_t* pList)
{
    auto pArrow = *reinterpret_cast<uint8_t**>(pList + arrow.nMember);
    if (!IsObject(pArrow))
        return;

    auto pArea = AsChildArea(pArrow);
    if (!pArea)
        return;

    auto nGlyphName = NameId(arrow.szGlyphName);
    auto pImage = FindNamedDeep(pArea, nGlyphName, 0);

    Rect content;
    auto bContent = ArrowContentBox(pArea, nGlyphName, content);

    auto nSprite = uintptr_t{ 0 };
    if (IsPadActiveDevice())
    {
        nSprite = GetShopSprite(arrow.szLetter);
        if (nSprite == 0 && GetPadButtonImage)
            nSprite = GetPadButtonImage(arrow.nFallbackButton);
    }

    if (nSprite == 0 || !bContent)
    {
        if (pImage)
            *reinterpret_cast<uintptr_t*>(pImage + nImageSprite) = 0;

        ShowArrowContent(pArea, nGlyphName, true);
        return;
    }

    if (!pImage)
        pImage = BuildImageChild(pArea, nGlyphName);

    auto pState = GetState(pImage);
    if (!pState)
    {
        ShowArrowContent(pArea, nGlyphName, true);
        return;
    }

    auto nBase = nCalibratedGlyphSide > 0 ? nCalibratedGlyphSide : nMenuGlyphSideFallback;
    auto nWide = nBase * nBazaarArrowQuadWide / nBazaarGlyphMeasured;
    auto nHigh = nBase * nBazaarArrowQuadHigh / nBazaarGlyphMeasured;
    auto nInset = nBase * nBazaarArrowInsetPixels / nBazaarGlyphMeasured;
    auto nNudge = nBase * nBazaarArrowNudgePixels / nBazaarGlyphMeasured;
    auto nDrop = nBase * nBazaarArrowDropPixels / nBazaarGlyphMeasured;

    if (nWide < 1 || nHigh < 1)
    {
        ShowArrowContent(pArea, nGlyphName, true);
        return;
    }

    // The pill on the box's inner edge, the empty texture beside it hanging over the bar.
    Rect rect;

    // The nudge stays inside the box's own span either way, so the width the list lays the arrow
    // out from does not change and the arrow does not walk away from it.
    if (arrow.bLeftOfBar)
    {
        rect.nRight = static_cast<int16_t>(content.nRight + nInset - nNudge);
        rect.nLeft = static_cast<int16_t>(rect.nRight - nWide);
    }
    else
    {
        rect.nLeft = static_cast<int16_t>(content.nLeft - nInset + nNudge);
        rect.nRight = static_cast<int16_t>(rect.nLeft + nWide);
    }

    rect.nTop = static_cast<int16_t>((content.nTop + content.nBottom) / 2 - nHigh / 2 + nDrop);
    rect.nBottom = static_cast<int16_t>(rect.nTop + nHigh);

    ResetImageState(pState);
    WriteRect(pState, rect);
    TintBazaarGlyph(pState);

    *reinterpret_cast<uint32_t*>(pImage + nWidgetLockMask) = nAllComponentsLocked;
    *reinterpret_cast<uintptr_t*>(pImage + nImageSprite) = nSprite;

    ShowArrowContent(pArea, nGlyphName, false);
}

static void ApplyBazaarArrows()
{
    auto pList = FindBazaarElement("BAZAAR_CATEGORY_LIST");
    if (!IsObject(pList))
        return;

    if (*reinterpret_cast<uintptr_t*>(pList) != Rva(nListBoxVtableRva))
        return;

    for (const auto& arrow : sBazaarArrows)
        ApplyBazaarArrow(arrow, pList);
}

// Everything on the computer again, for a device change rather than a focus change. RefreshFocus
// is the only other thing that places these and does not run when the input device flips. Gated
// on the page being open: the bazaar elements resolve from anywhere.
static void RefreshBazaarGlyphs()
{
    if (!bShopGlyphsOnPrompts)
        return;

    for (const auto& button : sBazaarButtons)
        ApplyBazaarGlyph(button);

    ApplyBazaarArrows();
}

class ControllerPrompts
{
public:
    ControllerPrompts()
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

                // The name to sprite calls, read off this function's own call sites.
                MakeSpriteName = reinterpret_cast<MakeSpriteName_t>(CallTarget(pMatch + nMakeSpriteNameCall));
                ResolveSprite = reinterpret_cast<ResolveSprite_t>(CallTarget(pMatch + nResolveSpriteCall));
                FreeSpriteName = reinterpret_cast<FreeSpriteName_t>(CallTarget(pMatch + nFreeSpriteNameCall));
            }

            // FUN_101D26D0's icon copy of the "_pc" suffix. Anchored on the "icon" push and the
            // ESP+0x54 string it goes into, which is what tells this copy from the show copy at
            // ESP+0x70 and the text copy at ESP+0x38.
            //
            //   68 ? ? ? ?      PUSH "icon"
            //   8D 4C 24 54     LEA  ECX,[ESP+0x54]
            //   E8 ? ? ? ?      CALL std::string::string
            //   68 ? ? ? ?      PUSH "_pc"                <- operand redirected
            //   8D 4C 24 1C     LEA  ECX,[ESP+0x1C]
            static constexpr ptrdiff_t nIconSuffixOperand = 0x0F;
            auto iconSuffixPattern = dunia_pattern("68 ? ? ? ? 8D 4C 24 54 E8 ? ? ? ? 68 ? ? ? ? 8D 4C 24 1C E8 ? ? ? ? 6A FF 6A 00 8D 4C 24 20 51 8D 4C 24 5C");
            if (!iconSuffixPattern.empty())
            {
                auto pSuffixOperand = iconSuffixPattern.get_first(nIconSuffixOperand);
                injector::WriteMemory<uintptr_t>(pSuffixOperand, reinterpret_cast<uintptr_t>(szXenonSuffix), true);
            }

            // Just past FUN_10189F50, which resolves the icon value through the document manager
            // and sets the button id to 0x5C on the way through whether it found a sprite or not.
            // Resolving the name here instead keeps the id and reuses FUN_105362E0. The
            // instruction is also the fall through for a missing icon attribute, where the value
            // string is empty and the match fails.
            //
            //   8D 84 24 A4 00 00 00   LEA  EAX,[ESP+0xA4]   ; the icon value
            //   50                     PUSH EAX
            //   8B CF                  MOV  ECX,EDI          ; the prompt
            //   E8 ? ? ? ?             CALL CNavBarPrompt::SetIconFromValue
            //   8D 8C 24 C0 00 00 00   LEA  ECX,[ESP+0xC0]   <- hook
            static constexpr ptrdiff_t nMenuIconApplied = 0x0F;
            static constexpr ptrdiff_t nMenuIconValue = 0xA4;
            auto menuIconPattern = dunia_pattern("8D 84 24 A4 00 00 00 50 8B CF E8 ? ? ? ? 8D 8C 24 C0 00 00 00 E8 ? ? ? ? 8B 16 8B 52 40");
            if (!menuIconPattern.empty() && GetPadButtonImage)
            {
                static auto MenuIconHook = safetyhook::create_mid(menuIconPattern.get_first(nMenuIconApplied), [](SafetyHookContext& regs)
                {
                    auto pPrompt = reinterpret_cast<uint8_t*>(regs.edi);
                    if (!pPrompt)
                        return;

                    auto pValue = reinterpret_cast<const uint8_t*>(regs.esp + nMenuIconValue);
                    auto nButton = PadButtonFromIconValue(StringView(pValue));
                    if (nButton < 0)
                        return;

                    auto nSprite = GetPadButtonImage(nButton);
                    if (nSprite == 0)
                        return;

                    *reinterpret_cast<int32_t*>(pPrompt + nPromptButtonId) = nButton;
                    *reinterpret_cast<uintptr_t*>(pPrompt + nPromptSprite) = nSprite;
                });
            }

            // FUN_10AB4F50, magma::Text's alignment offset. See the pattern's own note above.
            static constexpr ptrdiff_t nTextAlignOffsetEntry = 0x27;
            auto textAlignPattern = dunia_pattern("8B F9 8B 74 24 54 85 F6 75 03 8B 77 08 8B 47 58 85 C0 75 07 33 C0 E9");
            if (!textAlignPattern.empty())
            {
                auto pMatch = reinterpret_cast<uint8_t*>(textAlignPattern.get_first());
                GetTextAlignOffset = reinterpret_cast<GetTextAlignOffset_t>(pMatch - nTextAlignOffsetEntry);
            }

            // CNavBarPrompt::OnAttach's tail call to SetIcon. The one point where the prompt is
            // attached, its element is known non null and the cached sprite is in hand.
            //
            //   56              PUSH ESI
            //   8B F1           MOV  ESI,ECX
            //   83 7E 04 00     CMP  dword [ESI+0x04],0
            //   C6 46 51 01     MOV  byte [ESI+0x51],1     ; attached
            //   74 3C           JZ   return
            //   83 7E 08 00     CMP  dword [ESI+0x08],0    ; the element
            //   74 36           JZ   return
            //   ...
            //   8B 46 40        MOV  EAX,[ESI+0x40]        ; the cached sprite
            //   50              PUSH EAX
            //   8B CE           MOV  ECX,ESI
            //   E8 ? ? ? ?      CALL CNavBarPrompt::SetIcon   <- hook
            static constexpr ptrdiff_t nNavBarSetIconCall = 0x44;
            auto navBarAttachPattern = dunia_pattern("56 8B F1 83 7E 04 00 C6 46 51 01 74 3C 83 7E 08 00 74 36 83 7E 3C 08 72 05 8B 46 28 EB 03 8D 46 28 50 E8");
            if (!navBarAttachPattern.empty())
            {
                static auto NavBarAttachHook = safetyhook::create_mid(navBarAttachPattern.get_first(nNavBarSetIconCall), [](SafetyHookContext& regs)
                {
                    auto pPrompt = reinterpret_cast<uint8_t*>(regs.esi);
                    if (!pPrompt)
                        return;

                    sAttachedPrompts.insert(pPrompt);
                    ApplyMenuGlyph(pPrompt);
                });

                /*
                  The attach is the wrong moment to measure, so the placement is redone every
                  frame instead of once.

                  Attaching runs CNavBarPrompt::SetText four instructions ahead of the SetIcon
                  hooked above, but with whatever the .desc carried, and the message box carries
                  no words: common.mgb.desc gives MESSAGEBOX its buttons and not its labels, so
                  "Cancel" and "Accept" arrive from the page afterwards. Measuring an empty label
                  is not an error the engine reports, it is a width of nothing, and a centred
                  label of no width puts both of its edges on the box's midpoint, which is where
                  the text lands. That is what put the glyphs on top of the words until a pad
                  press refreshed them.

                  Hooking the tail of SetText, so the label is in the widget first, was tried and
                  did not take. Every path that sets a label goes through FUN_10536270 and only
                  SetText calls it, so the lever is the right one; what it does not answer is
                  which of SetText and OnAttach runs last, and the glyph is only right if the
                  measurement is the later of the two.

                  Redrawing answers it without having to know. magma::ScreenManager::Draw runs the
                  whole UI once a frame, so a placement taken here is measured against whatever
                  the label says now. It costs a tree walk and a text measurement for at most four
                  prompts, and nothing at all outside a menu, where the set is empty. The HUD is
                  already placed this way for the same reason: its icon animates in, and a size
                  taken from the first frame stays wrong.

                  Hooked past the register saves rather than on the entry, which is a call target.

                    51 53 55 56 57    PUSH ECX / EBX / EBP / ESI / EDI
                    8B F9             MOV  EDI,ECX          <- hook
                    8B 0D ? ? ? ?     MOV  ECX,[the quad builder]
                    8B 01 8B 50 1C    its vtable, +0x1C
                    6A 01 FF D2       CALL it with 1
                */
                static constexpr ptrdiff_t nUiDrawEntry = 0x05;
                auto uiDrawPattern = dunia_pattern("51 53 55 56 57 8B F9 8B 0D ? ? ? ? 8B 01 8B 50 1C 6A 01 FF D2 8B 0D ? ? ? ? 8B 35 ? ? ? ? E8");
                if (!uiDrawPattern.empty())
                {
                    static auto UiDrawHook = safetyhook::create_mid(uiDrawPattern.get_first(nUiDrawEntry), [](SafetyHookContext&)
                    {
                        RefreshMenuGlyphs();
                    });
                }

                // Fired from the input drivers, so this lands on whichever thread saw the input.
                // The writes are a sprite pointer, a rect and two flag words, so the worst a race
                // with the draw costs is one frame of a half applied prompt.
                onInputDeviceChange() += []()
                {
                    RefreshMenuGlyphs();
                    RefreshBazaarGlyphs();
                };
            }

            // FUN_10AB75C0. The entry is unique on its own prologue, which loads the slot before
            // the register saves and reads it back as a byte once they are done.
            //
            //   83 EC 14        SUB  ESP,0x14
            //   8B 44 24 18     MOV  EAX,[ESP+0x18]   ; the slot, as a dword
            //   53 56           PUSH EBX / ESI
            //   ...
            //   8A 5C 24 20     MOV  BL,[ESP+0x20]    ; the same slot, low byte
            //   38 4C 24 24     CMP  [ESP+0x24],CL    ; the enable flag
            // CHudPromptMgr::Update's per prompt loop. Hooking the call hands ECX over as the
            // prompt.
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

                    // Nothing requested means the prompt is on its way out and its group hidden.
                    if (*reinterpret_cast<uint32_t*>(pPrompt + nHudPromptShown) == 0)
                        return;

                    auto pEntry = FindHudPrompt(*reinterpret_cast<uint32_t*>(pPrompt + nHudPromptName));
                    if (pEntry)
                        ApplyHudGlyph(pPrompt, *pEntry);
                });
            }

            // The ui/360.mgb document, caught on the glyph loader's own load. It runs for every
            // menu and HUD glyph, so the pointer is there long before the shop opens.
            //
            //   E8 ? ? ? ?      CALL FUN_105355B0
            //   83 7C 24 24 10  CMP  dword [ESP+0x24],0x10   <- hook, EAX is the document
            if (!glyphPattern.empty())
            {
                static auto UiDocumentHook = safetyhook::create_mid(glyphPattern.get_first(nUiDocumentLoaded), [](SafetyHookContext& regs)
                {
                    pUiDocument = reinterpret_cast<void*>(regs.eax);
                });
            }

            // CBazaarComputerUI::RefreshFocus. It runs on both pages and on every focus change,
            // which is often enough that the glyphs follow a device switch without a second hook.
            //
            //   83 EC 08        SUB  ESP,0x8
            //   53 55 56 57     PUSH EBX,EBP,ESI,EDI
            //   8B F9           MOV  EDI,ECX          ; the UI
            //   83 BF 80 01 00 00 00  CMP dword [EDI+0x180],0    <- hook
            auto bazaarPattern = dunia_pattern("83 EC 08 53 55 56 57 8B F9 83 BF 80 01 00 00 00 0F 84 ? ? ? ? 83 BF 84 01 00 00 00");
            if (!bazaarPattern.empty())
            {
                auto pBazaar = reinterpret_cast<uint8_t*>(bazaarPattern.get_first());
                MakeElementName = reinterpret_cast<MakeElementName_t>(CallTarget(pBazaar + nMakeElementNameCall));
                FindElement = reinterpret_cast<FindElement_t>(CallTarget(pBazaar + nFindElementCall));

                static auto BazaarHook = safetyhook::create_mid(bazaarPattern.get_first(nBazaarRefreshThis), [](SafetyHookContext&)
                {
                    for (const auto& button : sBazaarButtons)
                        ApplyBazaarGlyph(button);

                    ApplyBazaarArrows();
                });
            }

            // FUN_10720880, the loose lookup FindBazaarElement falls back on. Anchored whole
            // because eight functions share its inner shape and differ only by a class global,
            // which a pattern cannot use once the dll moves.
            //
            //   8B 44 24 04           MOV  EAX,[ESP+4]   ; the name
            //   85 C0 56 8B F1 74 1A  null check, MOV ESI,ECX
            //   50 8D 44 24 0C 50
            //   E8 ? ? ? ?            CALL FUN_10AA7150  ; name object
            //   83 C4 08 50 8B CE
            //   E8 ? ? ? ?            CALL FUN_10181390  ; the lookup
            //   5E C2 04 00
            auto findByNamePattern = dunia_pattern("8B 44 24 04 85 C0 56 8B F1 74 1A 50 8D 44 24 0C 50 E8 ? ? ? ? 83 C4 08 50 8B CE E8 ? ? ? ? 5E C2 04 00");
            if (!findByNamePattern.empty())
                FindElementByName = reinterpret_cast<FindElementByName_t>(findByNamePattern.get_first());

            // CBazaarComputerUI's overrides of enter and leave on the menu page base, so the
            // menus behind the computer are never left wearing its glyphs.
            //
            //   56                    PUSH ESI                     <- enter, FUN_10734630
            //   8B F1                 MOV  ESI,ECX
            //   E8 ? ? ? ?            CALL the base enter
            //   80 BE 80 02 00 00 00  CMP  byte [ESI+0x280],0
            auto bazaarEnterPattern = dunia_pattern("56 8B F1 E8 ? ? ? ? 80 BE 80 02 00 00 00 0F 84 ? ? ? ? A1 ? ? ? ? 53 57 6A 01 8B CE");
            if (!bazaarEnterPattern.empty())
            {
                // No refresh here. The page's own prompts attach after this returns and read the
                // flag on the way in.
                static auto BazaarEnterHook = safetyhook::create_mid(bazaarEnterPattern.get_first(), [](SafetyHookContext&)
                {
                    bShopGlyphsOnPrompts = true;
                });
            }

            //   83 EC 30              SUB  ESP,0x30                <- leave, FUN_10731EA0
            //   56 57                 PUSH ESI,EDI
            //   8B F9                 MOV  EDI,ECX
            //   83 7F 6C 02           CMP  dword [EDI+0x6C],2
            auto bazaarLeavePattern = dunia_pattern("83 EC 30 56 57 8B F9 83 7F 6C 02 0F 84 ? ? ? ? 6A 00 E8");
            if (!bazaarLeavePattern.empty())
            {
                // At the first instruction, so the tree is still whole and the refresh has
                // something to write to.
                static auto BazaarLeaveHook = safetyhook::create_mid(bazaarLeavePattern.get_first(), [](SafetyHookContext&)
                {
                    bShopGlyphsOnPrompts = false;
                    RefreshMenuGlyphs();
                });
            }

            auto setCursorEnabledPattern = dunia_pattern("83 EC 14 8B 44 24 18 53 56 8B F1 50 8D 4C 24 0C 51 8B CE E8 ? ? ? ? 8B 10 8A 5C 24 20 33 C9 38 4C 24 24");
            if (!setCursorEnabledPattern.empty())
                SetCursorEnabled = reinterpret_cast<SetCursorEnabled_t>(setCursorEnabledPattern.get_first());

            // FUN_104F0FB0, the UI input dispatcher, for the timing rather than for anything it
            // holds. Per UI event rather than on the device change alone, because the game
            // re-enables its cursors on screen transitions; asking again once they are already
            // off costs one byte read.
            auto uiInputPattern = dunia_pattern("A1 ? ? ? ? 83 EC 24 53 56 33 DB 38 58 69 57 8B F9 74 1D 8B 48 78 3B CB 74 16");
            if (!uiInputPattern.empty() && SetCursorEnabled)
            {
                static auto UiInputHook = safetyhook::create_mid(uiInputPattern.get_first(), [](SafetyHookContext&)
                {
                    // The dispatcher sees key events, so the press that flips this is the same
                    // event that gets it applied.
                    static auto bToggleWasDown = false;
                    auto bToggleDown = (GetAsyncKeyState(nShopGlyphToggleKey) & 0x8000) != 0;

                    if (bToggleDown && !bToggleWasDown)
                    {
                        bShopGlyphsOnPrompts = !bShopGlyphsOnPrompts;
                        RefreshMenuGlyphs();
                    }

                    bToggleWasDown = bToggleDown;

                    // FUN_10AB75C0 reaches back into the event machinery to fire the leave.
                    if (bInCursorUpdate)
                        return;

                    bInCursorUpdate = true;

                    if (IsPadActiveDevice())
                        DisableCursors();
                    else
                        RestoreCursors();

                    bInCursorUpdate = false;
                });
            }
        };
    }
} ControllerPrompts;
