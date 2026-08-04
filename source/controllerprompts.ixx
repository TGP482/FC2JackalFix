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

// If the point size cannot be read. 150 units was what the counted build fell back to, so it
// takes the same 30 over 11 and lands on the same square whichever branch runs.
static constexpr int nMenuGlyphSideFallback = 409;

// Beside the label.
static constexpr int nMenuGlyphGapPercent = 40;

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

// Console puts the glyph outboard, and the slots are fixed: b_prompt1 is the rightmost and
// b_prompt4 the leftmost. Console's display options splits four that way, "(B) Back" b_prompt4
// and "(X) Default" b_prompt3 in front of their labels against "Apply (Y)" b_prompt2 and
// "Accept (A)" b_prompt1 behind. The node the prompt hangs off carries the slot's name crc.
//
// Comparing element rects across the prompt set was the first attempt and gets a lone prompt
// wrong: the main menu shows only "Accept (A)", and with nothing to span, a midpoint puts it in
// front of the label where console puts it behind.
static bool GlyphSitsRight(uint8_t* pPrompt)
{
    auto pNode = *reinterpret_cast<uint8_t**>(pPrompt + nPromptNode);
    if (!pNode)
        return false;

    static const auto nFirstSlot = NameId("b_prompt1");
    static const auto nSecondSlot = NameId("b_prompt2");

    auto nName = *reinterpret_cast<uint32_t*>(pNode + nNodeNameId);
    return nName == nFirstSlot || nName == nSecondSlot;
}

static bool ComputeMenuGlyphRect(uint8_t* pArea, bool bRightSide, Rect& out)
{
    static const auto nLabelName = NameId("t_button_text");

    auto pText = FindNamedDeep(pArea, nLabelName, 0);
    auto pTextState = GetState(pText);
    auto box = ReadRect(pTextState);
    if (!box.Valid())
        return false;

    auto nSide = nMenuGlyphSideFallback;

    auto fPointSize = *reinterpret_cast<float*>(pTextState + nTextStatePointSize);
    if (fPointSize >= 1.0f && fPointSize <= 4096.0f)
        nSide = static_cast<int>(fPointSize * nMenuGlyphSizeNumerator / nMenuGlyphSizeDenominator);

    if (nSide < 1)
        return false;

    auto nGap = nSide * nMenuGlyphGapPercent / 100;
    auto nCentreY = (box.nTop + box.nBottom) / 2;

    out.nTop = static_cast<int16_t>(nCentreY - nSide / 2);
    out.nBottom = static_cast<int16_t>(out.nTop + nSide);

    // Without a font there is nothing to measure and nothing to sit against, so the box edge
    // stands in and a centred label leaves a gap until the next attach.
    auto nBoxWidth = box.nRight - box.nLeft;
    auto nTextWidth = MeasureTextWidth(pText, nBoxWidth);
    auto bMeasured = nTextWidth >= 0;

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
static void ApplyMenuGlyph(uint8_t* pPrompt)
{
    auto pElement = *reinterpret_cast<uint8_t**>(pPrompt + nPromptElement);
    if (!pElement)
        return;

    auto pArea = *reinterpret_cast<uint8_t**>(pElement + nWidgetChildArea);
    if (!pArea)
        return;

    // Not "i_placeholder". Under that name FUN_10189BA0 would find the slot on the next attach
    // and push prompt+0x40 straight back into it, undoing the blank on the way to keyboard.
    static const auto nGlyphName = NameId("i_jackalfix_glyph");
    static const auto nBackgroundName = NameId("i_background");

    auto pImage = FindNamedDeep(pArea, nGlyphName, 0);
    auto pBackground = FindNamedNode(pArea, nBackgroundName);

    auto nSprite = *reinterpret_cast<uintptr_t*>(pPrompt + nPromptSprite);

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

    Rect rect;
    if (!ComputeMenuGlyphRect(pArea, GlyphSitsRight(pPrompt), rect))
        return;

    ResetImageState(pState);
    WriteRect(pState, rect);

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

                // Fired from the input drivers, so this lands on whichever thread saw the input.
                // The writes are a sprite pointer, a rect and two flag words, so the worst a race
                // with the draw costs is one frame of a half applied prompt.
                onInputDeviceChange() += []()
                {
                    RefreshMenuGlyphs();
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
