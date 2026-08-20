module;

#include <common.hxx>
#include <cstdint>
#include <vector>

export module controllerprompts;

import common;
import dunia;
import inputdevice;
import settings;

// The whole module turns on one question, whether the pad is the device in use, and every branch
// that asks it already has the "no" answer written out. So the setting is that question with one
// more term in it. Read where it is used rather than pushed anywhere, since the nav bar is walked
// once a frame and the HUD once per prompt update.
static bool bControllerPrompts = true;

// The one question this module asks, with the setting folded into it.
static bool PadPromptsWanted()
{
    return bControllerPrompts && IsPadActiveDevice();
}

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

// magma::ListBox and magma::RectShape, named by their own RTTI. The shop page is built out of
// them, so without both on the rect-state list the arrows read no box off anything.
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

/*
  Non-null was the only test these walks applied, and a crash dump says that is not enough:

      Access violation reading location 0x00000007
      EAX 0xFFFFFFFF   => ForEachChild<FindNamedDeep's lambda>+0x98

  0xFFFFFFFF passed the null check and 0xFFFFFFFF + nNodeNameId wraps to 7, which is the fault
  address. Arithmetic rather than IsReadable on purpose: these recurse over whole subtrees from a
  per frame refresh, and anything that gets past the range and alignment test would get past a
  VirtualQuery too.
*/
static bool IsPlausiblePointer(const void* pAddress)
{
    const auto n = reinterpret_cast<uintptr_t>(pAddress);

    // Above the null page, below the end of the user half, and aligned. Every magma object is.
    return n >= 0x10000 && n < 0x7FFF0000 && (n & 3) == 0;
}

// Walks an Area's direct children. The 64 cap is a sanity bound on a vector that may be garbage,
// not a format limit.
template <typename F>
static void ForEachChild(uint8_t* pArea, F&& fn)
{
    if (!IsPlausiblePointer(pArea))
        return;

    auto ppBegin = *reinterpret_cast<uint8_t***>(pArea + nAreaChildBegin);
    auto ppEnd = *reinterpret_cast<uint8_t***>(pArea + nAreaChildEnd);
    if (!IsPlausiblePointer(ppBegin) || !IsPlausiblePointer(ppEnd) || ppEnd < ppBegin)
        return;

    auto nCount = static_cast<size_t>(ppEnd - ppBegin);
    if (nCount > 64)
        return;

    for (size_t i = 0; i < nCount; ++i)
    {
        auto pNode = ppBegin[i];
        if (!IsPlausiblePointer(pNode))
            continue;

        auto nId = *reinterpret_cast<uint32_t*>(pNode + nNodeNameId);
        auto pDrawable = *reinterpret_cast<uint8_t**>(pNode + nNodeDrawable);
        if (IsPlausiblePointer(pDrawable))
            fn(nId, pDrawable);
    }
}

// The same walk, handing over the Node as well. Visibility lives on the Node and the rect on the
// drawable behind it, so anything that has to hide what it measures needs both.
template <typename F>
static void ForEachChildNode(uint8_t* pArea, F&& fn)
{
    if (!IsPlausiblePointer(pArea))
        return;

    auto ppBegin = *reinterpret_cast<uint8_t***>(pArea + nAreaChildBegin);
    auto ppEnd = *reinterpret_cast<uint8_t***>(pArea + nAreaChildEnd);
    if (!IsPlausiblePointer(ppBegin) || !IsPlausiblePointer(ppEnd) || ppEnd < ppBegin)
        return;

    auto nCount = static_cast<size_t>(ppEnd - ppBegin);
    if (nCount > 64)
        return;

    for (size_t i = 0; i < nCount; ++i)
    {
        auto pNode = ppBegin[i];
        if (!IsPlausiblePointer(pNode))
            continue;

        auto pDrawable = *reinterpret_cast<uint8_t**>(pNode + nNodeDrawable);
        if (IsPlausiblePointer(pDrawable))
            fn(*reinterpret_cast<uint32_t*>(pNode + nNodeNameId), pNode, pDrawable);
    }
}

// Only these carry a magma::State at +0x08. Reading a rect off anything else crashes: Button at
// 0xEE800C keeps something else there, and the build that listed it faulted twice.
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
  The PC prompt has no glyph child, so ask magma's factory for one rather than editing the archive:

      Factory::CreateElementForType(ti)  0xABF0E0  Element plus drawable with a constructed State.
                                                   The raw Image factory leaves State uninitialised.
      Factory::CreateKeyframe(ti)        0xABEE80
      Element::AddKeyframe(kf)           0xAB1C10
      Area::AddElement(e)                vtable +0x48
      Area::SetTime(0,0,0)               0xA973E0  forces one evaluation

  The Area destructor frees the Element, so one from our own CRT would hand a foreign pointer to
  magma's pool free. The keyframe is not optional: an empty keyframe vector gets index 0xFFF and the
  draw gate refuses it. Contents do not matter, the components are pinned afterwards.
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

// FUN_10533F10's names in id order. Matching an icon value against these and going back through
// FUN_105362E0 keeps the glyph on one loader and leaves the document manager out of it.
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

// A .mgb.desc attribute value. Anything longer is a bad read rather than a long value.
static constexpr uint32_t nStringSizeLimit = 0x400;

// Console draws the glyph 30x30 at 720p. Rect units are not pixels, so the square is calibrated:
// a glyph at 6/5 of the label's point size counted 11 px across at 1280x720, so 36/11 of the point
// size. All 33 glyph textures are 64x64, so one square suits every button.
static constexpr ptrdiff_t nTextStatePointSize = 0x40;
static constexpr int nMenuGlyphSizeNumerator = 36;
static constexpr int nMenuGlyphSizeDenominator = 11;
static constexpr int nMenuGlyphPixels = 30;

// The one length in rect units whose size on screen is known, so everything else is measured
// against it. The HUD has no label to derive a point size from and borrows this.
static int32_t nCalibratedGlyphSide = 0;

// If the point size cannot be read. 150 units was the counted build's fallback, so 36/11 of it
// lands on the same square whichever branch runs.
static constexpr int nMenuGlyphSideFallback = 409;

// Beside the label. Counted on a 720p console capture of the display options: 7 px on "(B) Back",
// 7 on "(X) Default", 10 on "Apply (Y)", 9 on "Accept (A)". The wider two are labels ending in a
// "y", which stops short of the glyph on its own, so the tighter end is the real one.
//
// Held against the disc rather than the unit space. Both are lengths across, so the scale divides
// out and the ratio survives any resolution. Fixed in units it would grow with the display's width
// while the glyph grows with its height.
static constexpr int nMenuGlyphGapPixels = 6;
static constexpr int nMenuGlyphRoundPixels = 31;

// The quad is not the glyph. The sprite carries a transparent margin and the rect sizes the quad,
// not the disc inside it, so the gap the player sees starts a margin further in.
//
// Solved rather than counted, neither margin edge being visible. Three builds asked for gaps of
// 0.4, 0.2 and 4/7 of the side and their captures read 43, 54 and 30 px between the "(B) Back"
// glyph and its label. Two unknowns, three readings: the quad comes out 65.0, 65.0 and 64.9 px with
// the disc at 34.0, 34.0 and 34.3, which is the 34 the disc measures directly. Held as a fraction
// of the quad, so both survive any size.
static constexpr int nMenuGlyphQuadPixels = 65;
static constexpr int nMenuGlyphDiscPixels = 34;

/*
  The unit space is not square on screen, which is the whole of the stretch. A square rect draws the
  disc 34 across against 31 down, and console draws the same sprite round, so the space carries the
  oval and the sprite does not.

  The space is fixed and stretched to the display, so the correction is one aspect against the
  other: 16/9 against 34/31 leaves the space itself 1.62 wide to 1 tall. Counting width against that
  puts 4:3 wider in units and 21:9 narrower, both square, and leaves height alone so the glyph still
  scales with the screen.

  Every round glyph takes it. The bazaar's category arrows do not: that quad is a pill counted whole
  off a capture with this stretch already in it.
*/
static constexpr int nUiSpaceWide = 162;
static constexpr int nUiSpaceHigh = 100;

// If the window cannot be measured. The space was solved at 1280x720, so falling back to it
// leaves the glyph exactly where the capture put it.
static constexpr int nFallbackScreenWide = 1280;
static constexpr int nFallbackScreenHigh = 720;

// Below this a client rect is a window being built or torn down rather than a display.
static constexpr int nScreenSizeFloor = 64;

// FUN_10AB4F50. Anchored past the SEH prologue, which is shared with every guarded function, on
// the two null checks that reach the font through Text+0x58.
//
//   8B 74 24 54  MOV  ESI,[ESP+0x54]   ; the State argument, null means the widget's own
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

// FUN_10AB4F50 returns where the string starts inside the rect, which for right alignment is box
// width less text width. Forcing the alignment for one call is the measurement; the field is a
// plain member, so nothing blends the original back in the meantime.
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
  Console puts the glyph outboard and the button picks the side, not the slot. Its quit box reads
  "(B) Cancel" against "Accept (A)"; its display options put A and Y behind their labels and B and X
  in front, so b_prompt2 lands on either side depending on what it carries.

  The slot decides only where there is no button, +0x4C reading 0x5C. b_prompt1 is rightmost.

  Comparing element rects across the prompt set gets a lone prompt wrong: the main menu shows only
  "Accept (A)", and with nothing to span a midpoint puts it in front of the label.
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
// ApplyMenuGlyph's keyboard branch, so reaching the computer without the pad having dressed a menu
// prompt left the side at zero and the shop fell back to the menu's square.
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

// Walked out of this process rather than asked for as the active window, since the glyph is placed
// from the input thread as well as the game thread and the active window is per thread.
static HWND hGameWindow = nullptr;

static BOOL CALLBACK TakeGameWindow(HWND hWindow, LPARAM)
{
    DWORD nProcess = 0;
    GetWindowThreadProcessId(hWindow, &nProcess);

    if (nProcess != GetCurrentProcessId() || !IsWindowVisible(hWindow))
        return TRUE;

    hGameWindow = hWindow;
    return FALSE;
}

static void ScreenSize(int32_t& nWide, int32_t& nHigh)
{
    nWide = nFallbackScreenWide;
    nHigh = nFallbackScreenHigh;

    if (!IsWindow(hGameWindow))
    {
        hGameWindow = nullptr;
        EnumWindows(TakeGameWindow, 0);
    }

    RECT client{};
    if (!hGameWindow || !GetClientRect(hGameWindow, &client))
        return;

    auto nClientWide = static_cast<int32_t>(client.right - client.left);
    auto nClientHigh = static_cast<int32_t>(client.bottom - client.top);

    if (nClientWide < nScreenSizeFloor || nClientHigh < nScreenSizeFloor)
        return;

    nWide = nClientWide;
    nHigh = nClientHigh;
}

// The width that draws as square as the side is tall, whatever the display is shaped like. Takes
// the side as an argument, since the three pages size their glyphs differently and share only shape.
static int32_t GlyphWidth(int32_t nSide)
{
    int32_t nScreenWide = 0;
    int32_t nScreenHigh = 0;
    ScreenSize(nScreenWide, nScreenHigh);

    auto nWide = nSide * nUiSpaceWide / nUiSpaceHigh;
    nWide = nWide * nScreenHigh / nScreenWide;

    return nWide > 0 ? nWide : nSide;
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

    auto nWide = GlyphWidth(nSide);

    // Both come off the width the display worked out rather than off the side, so the glyph keeps
    // its proportions wherever it lands. Taking the margin here leaves the gap measured to the
    // disc.
    auto nDisc = nWide * nMenuGlyphDiscPixels / nMenuGlyphQuadPixels;
    auto nMargin = (nWide - nDisc) / 2;
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

    out.nLeft = static_cast<int16_t>(bRightSide ? nTextRight + nGap : nTextLeft - nGap - nWide);
    out.nRight = static_cast<int16_t>(out.nLeft + nWide);
    return true;
}

// Runs at the SetIcon call in CNavBarPrompt::OnAttach, and again for every attached prompt when the
// active device flips. Forward declarations, all defined below.
static bool IsReadable(const void* pAddress);
static bool IsObject(uint8_t* pObject);
static bool ComputeBazaarGlyphRect(uint8_t* pLabel, Rect& out);
static void TintBazaarGlyph(uint8_t* pState);
static const char* ShopLetterFromButton(int32_t nButton);
static uintptr_t GetShopSprite(const char* szLetter);

/*
  Whether the computer's own message boxes wear its glyphs. On at page enter, off at page leave.

  Deciding it from the prompt does not work: a bazaar button resolves from anywhere, so asking
  whether one exists says yes in every menu, and the owner chain from a prompt carries no page name.
  A flag set from CBazaarComputerUI's destructor never clears, the object outliving its page.

  The page carries it. The menu page base vtable at 0xF4747C takes enter at slot 2 and leave at slot
  3, and CBazaarComputerUI overrides both (FUN_10734630, FUN_10731EA0). Reading a state off the
  object instead does not work either: its page pointers at +0x180 and +0x184 are never zeroed.
*/
static constexpr int nShopGlyphToggleKey = VK_F10;
static bool bShopGlyphsOnPrompts = false;

static void ApplyMenuGlyph(uint8_t* pPrompt)
{
    // IsLivePrompt vouches for the prompt rather than for what it points at. A prompt can be
    // attached and still hold a stale element: a crash came back with 0x21 in hand, faulting on
    // 0x5D, which is that plus the child area offset, read straight through a null check.
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

    if (!PadPromptsWanted() || nSprite == 0)
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
  Nothing drives the nav bar per frame. CNavBarModule only touches a prompt on attach, so a row
  already on screen when the player puts the pad down would keep its glyphs until they navigated.
  Prompts attached since the last page change are kept and walked again on the device change.

  The pointers go stale: a teardown frees the vector and PromptSet::FindOrCreate grows it, moving
  every prompt without detaching anything. CNavBarPrompt opens with a vftable, which tells a live
  object from a freed block, and OnDetach clears the attached flag at +0x51.
*/
// A vector rather than a set: the pad buttons below want the most recently attached prompt for a
// button id, and attach order is the only thing that tells a modal's prompts from the page's.
static std::vector<uint8_t*> sAttachedPrompts;

// VirtualQuery rather than an SEH frame, so the check costs the same whether the page is there or
// not and the walk stays an ordinary function.
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

// Readable is not real. A crash came back faulting on 0x5D with 0x21 in hand, which is 0x21 plus
// the child area offset: something small and not a pointer arrived where an element should have
// been. Every vtable in this tree lives in Dunia, so one read rejects a small integer outright.
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
  The glyph is built as an Image here too, for the same reason as the nav bar: a dump of every HUD
  prompt subtree found a_prompt_interact and t_button absent and not one magma::Text anywhere, only
  Placeholders, AreaInstances and Images.

  The same dump gave the anchors to hang it off, by crc:

      interact       a_interact_icon 7AD39F66 -> stain CB2D676F, i_use_icon FC09AC1D
      switchweapon   a_weapon_switch 00E1C03A -> a_swap_icon 0509A605 -> i_stain 7395FC18,
                     i_arrow 608F7549
      inventory      a_inventory_icons 7A3A3A37 -> stain, i_watch E8B3D151, i_wrench A9264364,
                     i_phone FCF70CAA, i_ied D43D37D9

  Prompt+0x0C is the Area itself, not an element wrapping one. Buttons come from the shipped console
  action map: use -> pad:a and pad:y, reload -> pad:x, tryuseied -> right trigger, heal -> left
  shoulder. Console interacts with Y.
*/

// CHudPrompt, stride 0x60 on the manager's array. +0x08 is the show request, +0x0C the Area the
// <Prompt path="..."> entry resolved to at layout build time.
static constexpr ptrdiff_t nHudPromptName = 0x04;
static constexpr ptrdiff_t nHudPromptShown = 0x08;
static constexpr ptrdiff_t nHudPromptArea = 0x0C;

// The call inside CHudPromptMgr::Update's per prompt loop, which hands the prompt over as ECX.
static constexpr ptrdiff_t nHudPromptUpdateCall = 0x0E;

// Square in 720p pixels, converted through the menu glyph's calibrated side. Until a menu prompt
// has been placed there is nothing to convert with, so the fallback is a share of the icon beneath.
static constexpr int nHudGlyphGapPercent = 20;
static constexpr int nHudGlyphFallbackPercent = 85;

/*
  Dead end: centring the glyph on the screen rather than on the icon beneath it.

  Console does centre on the screen. On 720p captures Xenia puts the interact glyph at x 628..651
  and the swap glyph at 624..653, both on 639, while the icons underneath are not symmetric about
  the screen at all. Centring on the anchor rect lands 2 px right on interact and 3 on the swap, and
  that gap is left open: two pixels are not worth a fragile placement.

  Two attempts failed. The first derived screen centre from the glyph calibration and its own sanity
  check rejected it every frame. The second read the canvas, which is worth keeping: it is not a
  constant anywhere, magma refills it from the active screen every frame, and FUN_104EFAD0 clamps a
  moved cursor against render context +0x34 and +0x36 with the other end a literal zero. Half that
  width still did not move the glyph, so these rects are local to a parent rather than absolute, and
  nothing here can see a parent.
*/

// Position and size do not convert at the same rate in this tree, which is not understood. The
// square is right: at 30 the interact glyph measures 26 across and at 34 the swap measures 30, both
// within a pixel or two of Xenia. The offset is not: two captures with pixel identical anchors put
// a requested 17 px drop at 33 and a requested 12 at 23, a straight line of slope two. So the
// offset rate is measured rather than modelled.
static constexpr int nHudOffsetHalving = 2;

// szAbove is both the sibling the glyph is added next to and the rect it is measured from. nDown
// is counted off Xenia: console leaves 43 px between the interact glyph and the hand against 57
// here, and 39 against 52 on the weapon swap. The inventory prompts have no capture and take
// interact's number.
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

// Reports the Area the icon was found in, since the glyph has to be its sibling.
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

// Re-run every update rather than latched: the icon animates in, so a square taken from its first
// frame is far too small and the lock mask then pins it there.
static bool ComputeHudGlyphRect(uint8_t* pAnchor, const HudPrompt& prompt, Rect& out)
{
    auto anchor = ReadRect(GetState(pAnchor));
    if (!anchor.Valid())
        return false;

    auto nHeight = anchor.nBottom - anchor.nTop;

    auto nSide = nCalibratedGlyphSide > 0
        ? nCalibratedGlyphSide * prompt.nPixels / nMenuGlyphPixels
        : nHeight * nHudGlyphFallbackPercent / 100;

    if (nSide < 1)
        return false;

    auto nGap = nHeight * nHudGlyphGapPercent / 100;

    auto nCentreX = (anchor.nLeft + anchor.nRight) / 2;

    auto nDrop = nCalibratedGlyphSide > 0
        ? prompt.nDown * nCalibratedGlyphSide / (nMenuGlyphPixels * nHudOffsetHalving)
        : 0;

    // Centred on the anchor, so the narrower width costs nothing but the stretch.
    auto nWide = GlyphWidth(nSide);

    out.nLeft = static_cast<int16_t>(nCentreX - nWide / 2);
    out.nRight = static_cast<int16_t>(out.nLeft + nWide);
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

    if (!PadPromptsWanted())
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
  Only the enabled bitmask is touched. Far Cry 2's own FUN_104EEB20 looks like the obvious call and
  is not: past disabling the slots it unregisters every cursor, dropping its input capture record,
  and clears a byte on each device record. Calling it cost pad navigation and pad vibration.

  FUN_10AB75C0(screenManager, slot, enabled) is the narrow lever underneath, one bit of +0x29, with
  the leave event fired first so nothing stays highlighted. The manager is reached through a global:

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
  Console draws these in the computer's green outline style and the art ships on PC, in ui/360.mgb
  rather than ui/weapon_bazaar.mgb. Its dependency list carries a second set beside the gold one:
  360_a_shop, 360_b_shop, 360_x_shop, 360_y_shop, 360_lb_shop, 360_rb_shop. Sprite names follow the
  texture names, so button_a becomes button_a_shop.

  FUN_105362E0 is the lookup, and only the name changes, so the document is taken off the engine's
  own return rather than asked for. Asking meant handing FUN_105355B0 a std::string, and a hand
  built one crashed inside the game's msvcr80.

      CALL FUN_105355B0        ; ui/360.mgb            <- document cached off this return
      CALL FUN_10533F10        ; button id -> name
      CALL FUN_100BD1D0        ; name object from a char*
      CALL FUN_10A99300        ; document resolves it
      CALL FUN_100BCF90        ; name object away again

  Placement, counted off a 360 capture: the glyph sits left of the text bar rather than inside it,
  and the bar stays. Hiding crc 0xC03AFD13 to make room takes the label with it.

  Buttons are asked for by name rather than through CBazaarComputerUI's cached label pointers, which
  are not the widgets on screen: searching from them finds the label for Checkout and none of the
  other four. FUN_10731100 does the lookup in two calls at fixed offsets, both pattern matched here.
  The label inside a button is crc 0x81FDCEB5.
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
  The computer's green, written out rather than borrowed. Borrowing fails three ways: a label's
  colour dims with focus, the cart on Checkout reads FF000000 with its green in the texture, and
  scaling a borrowed value swaps red and blue between the two candidate byte orders.

  Byte order, measured: writing 0xFF87FFD0 rendered about 149,200,98. In memory that is D0 FF 87 FF,
  and reading red, green, blue, alpha gives 208,255,135, whose ratios match. So it is 0xAABBGGRR.

  The value is the console glyph's hue at full brightness. Sampling gave 4CB381 and writing that
  made the glyph vanish, a capture carrying the page's dimming and a tint multiplying. What survives
  sampling is the ratio 0.30 to 0.70 to 0.51, scaled until green is full.
*/
static constexpr uint32_t nBazaarGlyphColour = 0xFFB7FF6C;

// Counted on a 1280x720 capture: the calibrated square written whole came out 63 pixels across.
static constexpr int nBazaarGlyphPixels = 30;
static constexpr int nBazaarGlyphMeasured = 63;

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

// The sprite name is inferred from the texture name, so more than one spelling is tried with the
// gold sprite behind them all. button_y_shop alone resolved to nothing on the last build.
static constexpr std::array<const char*, 2> sShopSuffixes = {{ "_shop", "" }};
static constexpr std::array<const char*, 2> sShopPrefixes = {{ "button_", "360_" }};

// The checkout page only spells its cancel the WEAPON_BAZAAR_BUTTON_ way. FUN_107330C0 resolves
// the rest from +0x1A8: the three shop names, then CANCEL_CHECKOUT, ADDREMOVE_CHECKOUT and
// CHECKOUT_CHECKOUT at +0x1B4 through +0x1BC, the last two with no WEAPON_ twin.
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
// like 0..0 by -24236..4334. Anything an order of magnitude outside the canvas is one of those.
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

// +0x28 and +0x2C on a class that is not an Area are whatever happens to sit there, so the pair is
// checked before either is walked.
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

// Most widgets hang an Area off +0x3C. magma::Button at 0xEE800C does not, it is one: its vtable
// takes Area::Draw at +0x44 and Area::AddElement at +0x48, both against the node vector on the
// object itself.
static uint8_t* AsChildArea(uint8_t* pDrawable)
{
    if (!pDrawable)
        return nullptr;

    auto pArea = *reinterpret_cast<uint8_t**>(pDrawable + nWidgetChildArea);
    if (LooksLikeArea(pArea))
        return pArea;

    return LooksLikeArea(pDrawable) ? pDrawable : nullptr;
}

// By drawable rather than by name, since the id a Node carries is the slot it fills and not the
// name the element was asked for.
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

// FUN_10730610 checks twice, the second time against a type at 0xEE7E90, and the checkout page's
// add/remove and checkout fail there: the names resolve, the drawables are the wrong class.
// FUN_107330C0 uses FUN_10720880 for those, the same lookup without the second check, which is the
// fallback below.
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

// BAZAAR_BUTTON_ADDREMOVE_CHECKOUT and BAZAAR_BUTTON_CHECKOUT_CHECKOUT resolve to objects with no
// Area and no child vector, each being a part of its button. The Area holding it is the button's,
// and carries both the label to measure from and the glyph.
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

// First spelling that resolves, suffix outermost, so a missing button_b_shop falls back to gold.
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

// The _shop art is a white outline and the green is the tint. All four corners, so no gradient.
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

    // The bazaar draws at a little over twice the menu's scale: the calibrated 65 units, 30 px in
    // the menu, measured 63 px here. Deriving from the label's point size was worse again, the
    // computer's font being the larger. So the calibration is carried across by that counted ratio.
    auto nBase = nCalibratedGlyphSide > 0 ? nCalibratedGlyphSide : nMenuGlyphSideFallback;

    auto nSide = nBase * nBazaarGlyphPixels / nBazaarGlyphMeasured;
    if (nSide < 1)
        return false;

    // Measured from the button's own left edge, which is zero, rather than from the label box:
    // Checkout's box begins at 33 because the cart sprite has the first 33, so hanging the glyph
    // off the box put Checkout's inside its bar and on top of the cart.
    //
    // A negative left is fine. A capture showing the glyph at -41..-11 was read as clipping once
    // and it was only dim, wearing an unfocused label's tint. The area does not clip, and outside
    // is where console has it.
    auto nGap = nBase * nBazaarGapPixels / nBazaarGlyphMeasured;
    auto nCentreY = (box.nTop + box.nBottom) / 2;

    // Hung off the bar's edge, so the narrower width takes the glyph in from the left and leaves
    // the counted two pixels where they were.
    auto nWide = GlyphWidth(nSide);

    out.nTop = static_cast<int16_t>(nCentreY - nSide / 2);
    out.nBottom = static_cast<int16_t>(out.nTop + nSide);
    out.nLeft = static_cast<int16_t>(-(nGap + nWide));
    out.nRight = static_cast<int16_t>(out.nLeft + nWide);
    return true;
}

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
    if (!PadPromptsWanted())
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

  Nothing to port. The xex names BAZAAR_CATEGORY_LIST and nothing on either side of it, so the LB
  and RB in the 360 capture are drawn by its own copy of ui/weapon_bazaar.mgb and by no code at all.

  They are not in the page tree either. A walk of BAZAAR_SHOP_PAGE six levels deep accounts for the
  whole page and finds no arrows, no page title and none of the slider's scroll arrows: they are
  members of the list rather than children of anything.
*/

// The pill does not fill its texture, so the square is worked back from a measurement. A 30 unit
// square drew it 19 by 18 against 28 by 26 for the round glyph in the same square, and a 58 by 33
// drew 34 by 20. It covers a little under three fifths either way, so console's 37 by 20 wants 63
// by 33 with 13 units of empty texture each side.
static constexpr int nBazaarArrowQuadWide = 63;
static constexpr int nBazaarArrowQuadHigh = 33;
static constexpr int nBazaarArrowWidePixels = 37;

static constexpr int nBazaarArrowInsetPixels =
    (nBazaarArrowQuadWide - nBazaarArrowWidePixels) / 2;

// Measured against Xenia. On the box's inner edge the pill cleared the bar by 5 left and 6 right
// against console's 9, and sat 2 px high, the "<<" box not quite centred on the bar.
static constexpr int nBazaarArrowNudgePixels = 4;
static constexpr int nBazaarArrowDropPixels = 2;

/*
  magma::ListBox, read off its own Draw at 0x10A9F590, and why the glyph is measured from the arrow
  rather than from the bar. The two arrows hang at +0x58 and +0x74, each drawn under a translate
  taken from a position pair the list writes itself out of the width of whatever the arrow holds,
  butting the arrow's right edge against the bar's left. Stock the arrow is 50 across; widened to
  128 by a glyph reaching to -78 it moved 74 further left, up beside the page title.

  Anchoring on the bar returns every unit of reach as an equal shift the other way, so the pill
  takes the inner edge of the arrow's own box with the empty 13 going over the bar.
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

// Console draws no box where the arrows are, only the pill, so the "<<" and its border come off
// with the pad in hand and go back without it.
static void ShowArrowContent(uint8_t* pArea, uint32_t nGlyphName, bool bVisible)
{
    ForEachChildNode(pArea, [&](uint32_t nId, uint8_t* pNode, uint8_t*)
    {
        if (nId != nGlyphName)
            SetNodeVisible(pNode, bVisible);
    });
}

// What the arrow covers in its own space: the union of the bordered box and the "<<" across it.
// Its own state rect is no use, a Button carrying nothing at +0x08 to read.
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
    if (PadPromptsWanted())
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

// For a device change rather than a focus change. RefreshFocus is the only other thing that places
// these and does not run when the device flips. Gated on the page being open, since the bazaar
// elements resolve from anywhere.
static void RefreshBazaarGlyphs()
{
    if (!bShopGlyphsOnPrompts)
        return;

    for (const auto& button : sBazaarButtons)
        ApplyBazaarGlyph(button);

    ApplyBazaarArrows();
}

// --------------------------------------------------------------------------------------------
// Menu buttons the PC build left on the mouse
// --------------------------------------------------------------------------------------------

/*
  Default, Apply, Create and Edit have a nav bar prompt and a pad glyph and no pad input: the only
  way to reach them is the pointer. The exit box is the same shape one step further, its Cancel and
  Accept reachable by pointer or by walking the focus onto them with the arrows, where console just
  wanted B or A.

  The pad does reach the UI. FUN_104F1A90 fills a map from action id to a UI key, keyed on the
  gamepad event type, and the ids are zlib CRC-32 of the action names, the same hash the widget
  tree uses:

      "a"  -> 0x0D    "x"     -> 0x202    "left_shoulder"  -> 0x206    "left_trigger"  -> 0x204
      "b"  -> 0x1B    "y"     -> 0x203    "right_shoulder" -> 0x207    "right_trigger" -> 0x205
                      "start" -> 0x20B    "back"           -> 0x20A

  So A and B arrive as Enter and Escape and land on whatever holds the focus, and X and Y arrive as
  0x202 and 0x203, which nothing in Dunia compares against: a search of every decoded instruction
  finds no CMP against either. The console handling is data, not code, and the PC .mgb does not
  carry it.

  What the pointer does is reachable. FUN_10AA5930, a page's mouse button handler, does not hit
  test: the hit test ran on the last move and left the Node it found at page+0x1C, and the press
  dispatches to it through that Node's vtable+0x8C. FUN_10AA23E0 is that hit test and it walks
  Nodes, which is what CNavBarPrompt keeps at +0x04. Its +0x08 is the widget the Node draws, and
  handing that over instead is a crash rather than a miss: a magma::ButtonInstance vftable is 21
  entries and +0x8C reads twenty past the end.

  Three things have to be right and none of them can be assumed:

      the page      Nothing in a prompt points back at it, and more than one page can reach the
                    same Node. The stack is walked and the tree of each page searched, top down.

      the position  A press is not done with it: the Node's own handler descends the widget with
                    it. magma::ButtonInstance keeps a position at State+0x24 where an Image or a
                    Text keeps a rect, and the label inside it is the one child whose State does
                    carry a rect, so the middle of the label is the point.

      the transform FUN_10AA16C0 takes a pair off the page's vtable+0x20 object and subtracts it on
                    the way in. Reading that pair back gives 160,40 while the engine subtracted
                    231,40 from the very press it was reading them for, so it is measured instead:
                    a move is sent with a known number and FUN_10AA5EC0, which receives the event
                    after the transform, is caught with the number the page ended up with.

  With those three the pointer's own hit test still does not pick the prompt up on the menu pages,
  only on the exit box, so the slot the press reads is written directly when a move does not land.
  Pages above the one being pressed are emptied for the two calls: FUN_10AB6690 walks top down and
  stops at the first page that handles the press, and a page with nothing hovered and nothing
  captured returns 0 from both handlers.

      manager + 0x04 / + 0x08   page stack, begin and end, 8 bytes a slot, page first
      manager + 0x28 / + 0x29   device and cursor bitmasks, both refused by the handlers when clear
      manager + 0x2C            pointer position per device, 8 bytes a slot, written by a press
      page    + 0x28 / + 0x2C   its own child vector, a Page being an Area
      page    + 100 + d * 0x3C  per device: + 0x1C hovered Node, + 0x20 captured

  A and B are only taken over on a message box. Everywhere else Enter already activates what the
  player walked to, and a page's "(A) Select" prompt is not always the same action, so pressing its
  glyph instead of the focused item would be a different game. On a box it is the point: the two
  prompts are the two buttons. The pad's A and B are cleared out of the driver's XINPUT_STATE for
  as long as one is open, so the action map cannot also send Enter and close the box twice.
*/

static constexpr uint32_t nMenuPadDevice = 0;

static constexpr ptrdiff_t nScreenStackBegin = 0x04;
static constexpr ptrdiff_t nScreenStackEnd = 0x08;
static constexpr ptrdiff_t nDeviceActiveMask = 0x28;
static constexpr ptrdiff_t nCursorPosition = 0x2C;
static constexpr size_t nScreenStackStride = 8;
static constexpr size_t nScreenStackLimit = 8;

static constexpr ptrdiff_t nScreenDeviceBase = 100;
static constexpr size_t nScreenDeviceStride = 0x3C;
static constexpr ptrdiff_t nScreenHovered = 0x1C;
static constexpr ptrdiff_t nScreenCaptured = 0x20;

// FUN_104F1A90 again, the mouse half of the same map: "lb" -> 1, "rb" -> 2.
static constexpr uint32_t nLeftMouseButton = 1;

// The event every handler takes: button, packed position, device byte, then a flag the dispatcher
// sets on everything it builds.
static constexpr size_t nMouseEventWords = 4;

static constexpr ptrdiff_t nStateOriginX = 0x24;
static constexpr ptrdiff_t nStateOriginY = 0x26;

static constexpr ptrdiff_t nMessageBoxVtableRva = 0xE1E3C8;

static constexpr uint16_t nXInputA = 0x1000;
static constexpr uint16_t nXInputB = 0x2000;
static constexpr uint16_t nXInputX = 0x4000;
static constexpr uint16_t nXInputY = 0x8000;

static constexpr uint16_t nNoPadButtonMask = 0xFFFF;

using UiMouseEvent_t = char(__thiscall*)(void*, uint32_t*);
static UiMouseEvent_t UiMouseMove = nullptr;
static UiMouseEvent_t UiMouseDown = nullptr;
static UiMouseEvent_t UiMouseUp = nullptr;

// CGameMessageBox's own constructor and destructor, which every box in the family runs. The
// derived classes overwrite the vftable straight after, so which kind of box this is is a question
// asked of the live object rather than of the hook.
static std::vector<uint8_t*> sLiveMessageBoxes;

static bool PlainMessageBoxOpen()
{
    for (auto* pBox : sLiveMessageBoxes)
    {
        if (IsReadable(pBox) && *reinterpret_cast<uintptr_t*>(pBox) == Rva(nMessageBoxVtableRva))
            return true;
    }

    return false;
}

// Filled by the hook on FUN_10AA5EC0 while a probe move is in flight.
static uint8_t* pCalibrateScreen = nullptr;
static int32_t nCalibrateX = 0;
static int32_t nCalibrateY = 0;
static bool bCalibrated = false;

// Two int16 in one word, x low. FUN_104EFAD0 packs the pointer's own the same way.
static uint32_t PackPosition(int32_t nX, int32_t nY)
{
    return (static_cast<uint32_t>(static_cast<uint16_t>(static_cast<int16_t>(nY))) << 16)
        | static_cast<uint16_t>(static_cast<int16_t>(nX));
}

// Whether a page holds the Node, and how far into it. FUN_10AA23E0 takes a child's rect off the
// position before descending into its sub tree, so a widget a frame down is not at its own origin
// as far as a press is concerned, and the walk that finds it adds up what it descended through.
static bool AreaHoldsNode(uint8_t* pArea, uint8_t* pWanted, int32_t& nOffsetX, int32_t& nOffsetY,
    int nDepth = 0)
{
    if (!pArea || nDepth > 8)
        return false;

    auto bFound = false;

    ForEachChildNode(pArea, [&](uint32_t, uint8_t* pNode, uint8_t* pDrawable)
    {
        if (bFound)
            return;

        if (pNode == pWanted)
        {
            bFound = true;
            return;
        }

        // +0x3C is a child Area on the classes that have one and whatever happens to sit there on
        // the ones that do not, so both ends go through the page tables rather than through the
        // shape of a pointer. A walk that took it on trust faulted three levels down reading
        // 0x22772228, which is 0x28 past a number that was aligned and in range and not mapped.
        if (!IsObject(pDrawable))
            return;

        auto pChild = *reinterpret_cast<uint8_t**>(pDrawable + nWidgetChildArea);
        if (!IsReadable(pChild) || !AreaHoldsNode(pChild, pWanted, nOffsetX, nOffsetY, nDepth + 1))
            return;

        bFound = true;

        if (auto pState = GetState(pDrawable))
        {
            nOffsetX += *reinterpret_cast<int16_t*>(pState + nStateOriginX);
            nOffsetY += *reinterpret_cast<int16_t*>(pState + nStateOriginY);
        }
    });

    return bFound;
}

static uint8_t** HoveredSlot(uint8_t* pScreen)
{
    return reinterpret_cast<uint8_t**>(
        pScreen + nScreenDeviceBase + nMenuPadDevice * nScreenDeviceStride + nScreenHovered);
}

// Backwards, so a modal's prompts win over whatever was attached behind it. The Node and the
// widget are checked as a pair: a Node whose drawable is not the prompt's widget is not the Node
// this prompt is drawn through, whatever else it may be.
static uint8_t* FindPrompt(int32_t nButton)
{
    for (auto it = sAttachedPrompts.rbegin(); it != sAttachedPrompts.rend(); ++it)
    {
        if (!IsLivePrompt(*it) || *reinterpret_cast<int32_t*>(*it + nPromptButtonId) != nButton)
            continue;

        auto pNode = *reinterpret_cast<uint8_t**>(*it + nPromptNode);
        auto pWidget = *reinterpret_cast<uint8_t**>(*it + nPromptElement);

        if (IsObject(pNode) && IsObject(pWidget)
            && *reinterpret_cast<uint8_t**>(pNode + nNodeDrawable) == pWidget)
        {
            return *it;
        }
    }

    return nullptr;
}

static bool PressPrompt(uint8_t* pPrompt)
{
    if (!UiMouseMove || !UiMouseDown || !UiMouseUp || !pPrompt)
        return false;

    auto pNode = *reinterpret_cast<uint8_t**>(pPrompt + nPromptNode);
    auto pWidget = *reinterpret_cast<uint8_t**>(pPrompt + nPromptElement);
    if (!IsObject(pNode) || !IsObject(pWidget))
        return false;

    auto pState = GetState(pWidget);
    auto pArea = pState ? *reinterpret_cast<uint8_t**>(pWidget + nWidgetChildArea) : nullptr;
    if (!IsReadable(pArea))
        return false;

    static const auto nLabelName = NameId("t_button_text");

    auto box = ReadRect(GetState(FindNamedDeep(pArea, nLabelName, 0)));
    if (!box.Valid())
        return false;

    auto pManager = ScreenManager();
    if (!pManager)
        return false;

    auto pBegin = *reinterpret_cast<uint8_t**>(pManager + nScreenStackBegin);
    auto pEnd = *reinterpret_cast<uint8_t**>(pManager + nScreenStackEnd);
    if (!pBegin || !pEnd || pEnd <= pBegin)
        return false;

    uint8_t* pPage = nullptr;
    int32_t nOffsetX = 0;
    int32_t nOffsetY = 0;

    for (size_t i = 0; i < nScreenStackLimit; ++i)
    {
        auto pSlot = pEnd - (i + 1) * nScreenStackStride;
        if (pSlot < pBegin)
            break;

        auto pScreen = *reinterpret_cast<uint8_t**>(pSlot);
        if (IsObject(pScreen) && AreaHoldsNode(pScreen, pNode, nOffsetX, nOffsetY))
        {
            pPage = pScreen;
            break;
        }
    }

    if (!pPage)
        return false;

    auto pCursor = reinterpret_cast<uint32_t*>(pManager + nCursorPosition + nMenuPadDevice * nScreenStackStride);
    auto nSavedCursor = *pCursor;
    auto nSavedActive = *(pManager + nDeviceActiveMask);
    auto nSavedEnabled = *(pManager + nCursorEnabledMask);

    // Every handler refuses a device that is not on, and this module turns the pointer off for the
    // pad. Written rather than set through FUN_10AB75C0, which fires a leave on the way.
    auto nBit = static_cast<uint8_t>(1 << nMenuPadDevice);
    *(pManager + nDeviceActiveMask) = static_cast<uint8_t>(nSavedActive | nBit);
    *(pManager + nCursorEnabledMask) = static_cast<uint8_t>(nSavedEnabled | nBit);

    // The probe goes to 0,0, which is off every page and so hits nothing on the way past. What
    // comes back is the transform and nothing else.
    bCalibrated = false;
    pCalibrateScreen = pPage;

    uint32_t probe[nMouseEventWords] = { 0, 0, nMenuPadDevice, 0 };
    UiMouseMove(pManager, probe);

    pCalibrateScreen = nullptr;

    auto bPressed = false;

    if (bCalibrated)
    {
        auto nPosition = PackPosition(
            nCalibrateX + nOffsetX + *reinterpret_cast<int16_t*>(pState + nStateOriginX) + (box.nLeft + box.nRight) / 2,
            nCalibrateY + nOffsetY + *reinterpret_cast<int16_t*>(pState + nStateOriginY) + (box.nTop + box.nBottom) / 2);

        uint32_t move[nMouseEventWords] = { 0, nPosition, nMenuPadDevice, 0 };
        UiMouseMove(pManager, move);

        auto ppHovered = HoveredSlot(pPage);
        auto bLanded = *ppHovered == pNode;
        auto pSavedHovered = *ppHovered;

        struct Emptied { uint8_t** ppHovered; uint8_t** ppCaptured; uint8_t* pHovered; uint8_t* pCaptured; };
        std::array<Emptied, nScreenStackLimit> emptied{};
        size_t nEmptied = 0;

        if (!bLanded)
        {
            for (size_t i = 0; i < nScreenStackLimit; ++i)
            {
                auto pSlot = pEnd - (i + 1) * nScreenStackStride;
                if (pSlot < pBegin)
                    break;

                auto pScreen = *reinterpret_cast<uint8_t**>(pSlot);
                if (pScreen == pPage)
                    break;

                if (!IsObject(pScreen))
                    continue;

                auto ppAbove = HoveredSlot(pScreen);
                auto ppCaptured = reinterpret_cast<uint8_t**>(
                    pScreen + nScreenDeviceBase + nMenuPadDevice * nScreenDeviceStride + nScreenCaptured);

                emptied[nEmptied++] = { ppAbove, ppCaptured, *ppAbove, *ppCaptured };
                *ppAbove = nullptr;
                *ppCaptured = nullptr;
            }

            *ppHovered = pNode;
        }

        uint32_t press[nMouseEventWords] = { nLeftMouseButton, nPosition, nMenuPadDevice, 1 };
        UiMouseDown(pManager, press);
        UiMouseUp(pManager, press);

        if (!bLanded && *ppHovered == pNode)
            *ppHovered = pSavedHovered;

        for (size_t i = 0; i < nEmptied; ++i)
        {
            *emptied[i].ppHovered = emptied[i].pHovered;
            *emptied[i].ppCaptured = emptied[i].pCaptured;
        }

        bPressed = true;
    }

    *(pManager + nDeviceActiveMask) = nSavedActive;
    *(pManager + nCursorEnabledMask) = nSavedEnabled;
    *pCursor = nSavedCursor;

    return bPressed;
}

struct MenuPadButton
{
    uint16_t nBit;
    int32_t nPromptButton;

    // A and B mean something already. See the note at the top of this section.
    bool bMessageBoxOnly;
};

static constexpr std::array<MenuPadButton, 4> sMenuPadButtons =
{{
    { nXInputA, nPadA, true },
    { nXInputB, nPadB, true },
    { nXInputX, nPadX, false },
    { nXInputY, nPadY, false },
}};

// Runs off the UI draw, which is the one place that is already walking the attached prompts and
// only runs while there is a menu to walk.
static void UpdateMenuPadButtons()
{
    static uint16_t nHeld = 0;

    if (!IsPadActiveDevice())
    {
        nHeld = 0;
        SetPadButtonMask(nNoPadButtonMask);
        return;
    }

    auto nButtons = GetPadState().nButtons;
    auto nPressed = static_cast<uint16_t>(nButtons & ~nHeld);
    nHeld = nButtons;

    auto bMessageBox = PlainMessageBoxOpen();
    auto nMask = nNoPadButtonMask;

    for (const auto& button : sMenuPadButtons)
    {
        if (button.bMessageBoxOnly != bMessageBox)
            continue;

        auto pPrompt = FindPrompt(button.nPromptButton);
        if (!pPrompt)
            continue;

        // Held back for as long as the prompt is there to take it, not just on the frame it is
        // pressed: the action map runs on its own poll and would otherwise get there first.
        if (button.bMessageBoxOnly)
            nMask = static_cast<uint16_t>(nMask & ~button.nBit);

        if (nPressed & button.nBit)
            PressPrompt(pPrompt);
    }

    SetPadButtonMask(nMask);
}

class ControllerPrompts
{
public:
    ControllerPrompts()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            bControllerPrompts = JackalFixSettings.GetInt(PREF_CONTROLLERPROMPTS) != 0;

            // Read where it is used, on the engine's own threads, which keeps this off the file
            // watcher's thread where touching a widget tree is not safe.
            JackalFix::onIniFileChange() += []()
            {
                bControllerPrompts = JackalFixSettings.GetInt(PREF_CONTROLLERPROMPTS) != 0;
            };

            // FUN_105362E0, the 360 glyph loader. Anchored on the two null checks inside it, its
            // prologue being std::string boilerplate shared with unrelated functions.
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
            // ESP+0x54 string it goes into, which tells this copy from the show copy at ESP+0x70
            // and the text copy at ESP+0x38.
            //
            //   68 ? ? ? ?      PUSH "icon"
            //   8D 4C 24 54     LEA  ECX,[ESP+0x54]
            //   68 ? ? ? ?      PUSH "_pc"                <- operand redirected
            static constexpr ptrdiff_t nIconSuffixOperand = 0x0F;
            auto iconSuffixPattern = dunia_pattern("68 ? ? ? ? 8D 4C 24 54 E8 ? ? ? ? 68 ? ? ? ? 8D 4C 24 1C E8 ? ? ? ? 6A FF 6A 00 8D 4C 24 20 51 8D 4C 24 5C");
            if (!iconSuffixPattern.empty())
            {
                auto pSuffixOperand = iconSuffixPattern.get_first(nIconSuffixOperand);
                injector::WriteMemory<uintptr_t>(pSuffixOperand, reinterpret_cast<uintptr_t>(szXenonSuffix), true);
            }

            // Just past FUN_10189F50, which resolves the icon value through the document manager
            // and sets the button id to 0x5C whether it found a sprite or not. Resolving the name
            // here keeps the id and reuses FUN_105362E0. The instruction is also the fall through
            // for a missing icon attribute, where the value string is empty and the match fails.
            //
            //   8D 84 24 A4 00 00 00   LEA  EAX,[ESP+0xA4]   ; the icon value
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

            // CNavBarPrompt::OnAttach's tail call to SetIcon, the one point where the prompt is
            // attached, its element is known non null and the cached sprite is in hand.
            //
            //   C6 46 51 01     MOV  byte [ESI+0x51],1     ; attached
            //   8B 46 40        MOV  EAX,[ESI+0x40]        ; the cached sprite
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

                    if (std::find(sAttachedPrompts.begin(), sAttachedPrompts.end(), pPrompt) == sAttachedPrompts.end())
                        sAttachedPrompts.push_back(pPrompt);
                    ApplyMenuGlyph(pPrompt);
                });

                /*
                  Attach is the wrong moment to measure, so the placement is redone every frame.

                  SetText runs four instructions ahead of the SetIcon hooked above, but with
                  whatever the .desc carried, and the message box carries no words: common.mgb.desc
                  gives MESSAGEBOX its buttons and not its labels, so "Cancel" and "Accept" arrive
                  from the page afterwards. Measuring an empty label is a width of nothing, and a
                  centred label of no width puts both edges on the box midpoint, which is where the
                  text lands. That put the glyphs on top of the words until a pad press refreshed.

                  Hooking the tail of SetText instead does not answer which of it and OnAttach runs
                  last, and the glyph is only right if the measurement is the later of the two.
                  Redrawing every frame answers it without having to know, at the cost of a tree
                  walk for at most four prompts and nothing at all outside a menu.

                  Hooked past the register saves rather than on the entry, which is a call target.
                */
                static constexpr ptrdiff_t nUiDrawEntry = 0x05;
                auto uiDrawPattern = dunia_pattern("51 53 55 56 57 8B F9 8B 0D ? ? ? ? 8B 01 8B 50 1C 6A 01 FF D2 8B 0D ? ? ? ? 8B 35 ? ? ? ? E8");
                if (!uiDrawPattern.empty())
                {
                    static auto UiDrawHook = safetyhook::create_mid(uiDrawPattern.get_first(nUiDrawEntry), [](SafetyHookContext&)
                    {
                        RefreshMenuGlyphs();
                        UpdateMenuPadButtons();
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

            // CHudPromptMgr::Update's per prompt loop. Hooking the call hands ECX over as the
            // prompt.
            //
            //   8B FF           MOV  EDI,EDI          ; hot patch pad, and a useful anchor
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

            // The ui/360.mgb document, caught on the glyph loader's own load, which runs for every
            // menu and HUD glyph long before the shop opens.
            if (!glyphPattern.empty())
            {
                static auto UiDocumentHook = safetyhook::create_mid(glyphPattern.get_first(nUiDocumentLoaded), [](SafetyHookContext& regs)
                {
                    pUiDocument = reinterpret_cast<void*>(regs.eax);
                });
            }

            // CBazaarComputerUI::RefreshFocus, which runs on both pages and on every focus change,
            // often enough that the glyphs follow a device switch without a second hook.
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

            // FUN_10720880, the loose lookup. Anchored whole because eight functions share its
            // inner shape and differ only by a class global, which a pattern cannot use.
            auto findByNamePattern = dunia_pattern("8B 44 24 04 85 C0 56 8B F1 74 1A 50 8D 44 24 0C 50 E8 ? ? ? ? 83 C4 08 50 8B CE E8 ? ? ? ? 5E C2 04 00");
            if (!findByNamePattern.empty())
                FindElementByName = reinterpret_cast<FindElementByName_t>(findByNamePattern.get_first());

            // CBazaarComputerUI's enter and leave overrides, so the menus behind the computer are
            // never left wearing its glyphs. FUN_10734630 and FUN_10731EA0.
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

            // FUN_104F0FB0, the UI input dispatcher, for the timing rather than anything it holds.
            // Per UI event rather than on the device change alone, since the game re-enables its
            // cursors on screen transitions.
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

                    if (PadPromptsWanted())
                        DisableCursors();
                    else
                        RestoreCursors();

                    bInCursorUpdate = false;
                });
            }

            // FUN_104F0FB0's own two calls into the screen manager, one after the other, so both
            // handlers come off one anchor and neither needs a prologue of its own.
            //
            //   8B 76 08        MOV  ESI,[ESI+0x08]
            //   3B 35 ? ? ? ?   CMP  ESI,[button down event type]
            //   75 1E           JNZ  the button up case
            //   8B 0D ? ? ? ?   MOV  ECX,[0x10FE3178]
            //   8D 54 24 10     LEA  EDX,[ESP+0x10]
            //   52              PUSH EDX
            //   83 C1 04        ADD  ECX,0x4
            //   E8 ? ? ? ?      CALL FUN_10AB6690          <- + 0x19
            //   ...
            //   E8 ? ? ? ?      CALL FUN_10AB6840          <- + 0x43
            static constexpr ptrdiff_t nUiMouseDownCall = 0x19;
            static constexpr ptrdiff_t nUiMouseUpCall = 0x43;
            auto uiMousePattern = dunia_pattern("8B 76 08 3B 35 ? ? ? ? 75 1E 8B 0D ? ? ? ? 8D 54 24 10 52 83 C1 04 E8");
            if (!uiMousePattern.empty())
            {
                auto pMatch = reinterpret_cast<uint8_t*>(uiMousePattern.get_first());
                UiMouseDown = reinterpret_cast<UiMouseEvent_t>(CallTarget(pMatch + nUiMouseDownCall));
                UiMouseUp = reinterpret_cast<UiMouseEvent_t>(CallTarget(pMatch + nUiMouseUpCall));
            }

            // The move, a few instructions ahead of the pair above and off the same global.
            //
            //   8B 46 04        MOV  EAX,[ESI+0x04]
            //   3B 05 ? ? ? ?   CMP  EAX,[move event type]
            //   75 1E           JNZ  past it
            //   8D 4C 24 10     LEA  ECX,[ESP+0x10]
            //   51              PUSH ECX
            //   8B 0D ? ? ? ?   MOV  ECX,[0x10FE3178]
            //   83 C1 04        ADD  ECX,0x4
            //   E8 ? ? ? ?      CALL FUN_10AB7580          <- + 0x19
            static constexpr ptrdiff_t nUiMouseMoveCall = 0x19;
            auto uiMovePattern = dunia_pattern("8B 46 04 3B 05 ? ? ? ? 75 1E 8D 4C 24 10 51 8B 0D ? ? ? ? 83 C1 04 E8");
            if (!uiMovePattern.empty())
            {
                auto pMatch = reinterpret_cast<uint8_t*>(uiMovePattern.get_first());
                UiMouseMove = reinterpret_cast<UiMouseEvent_t>(CallTarget(pMatch + nUiMouseMoveCall));
            }

            /*
              FUN_10AA5EC0, a page's own mouse move, past its prologue where ECX is still the page
              and the event is the stack argument. The event has already been through
              FUN_10AA16C0 by this point, so what it carries is the page's own number and the
              difference from what was sent is the transform.

                8B F9                 MOV   EDI,ECX                <- hook
                8B B4 24 84 ...       MOV   ESI,[ESP+0x84]         ; the event
                0F B6 46 08           MOVZX EAX,byte [ESI+0x08]    ; the device
            */
            static constexpr ptrdiff_t nPageMoveEvent = 0x84;
            if (auto* pPageMouseMove = dunia_find("8B F9 8B B4 24 84 00 00 00 0F B6 46 08 8B C8 C1 E1 04 2B C8 8D 6C 8F 64 8A 4D 38"))
            {
                static auto PageMouseMoveHook = safetyhook::create_mid(pPageMouseMove, [](SafetyHookContext& regs)
                {
                    if (!pCalibrateScreen || reinterpret_cast<uint8_t*>(regs.ecx) != pCalibrateScreen)
                        return;

                    auto pEvent = *reinterpret_cast<uint32_t**>(regs.esp + nPageMoveEvent);
                    if (!IsReadable(pEvent))
                        return;

                    // The probe carries 0,0, so what the page ended up with is the transform
                    // negated and nothing has to be remembered from the send.
                    nCalibrateX = -static_cast<int16_t>(pEvent[1] & 0xFFFF);
                    nCalibrateY = -static_cast<int16_t>(pEvent[1] >> 16);
                    bCalibrated = true;
                });
            }

            // CGameMessageBox::CGameMessageBox. ECX is the box, and the base vftable it writes here
            // is replaced by whichever class is actually being built, which is the point: the kind
            // is read later, off the live object.
            if (auto* pMessageBoxCtor = dunia_find("83 EC 14 8B 44 24 18 53 55 56 57 50 8B F1 E8 ? ? ? ? C7 46 70 ? ? ? ? 8D 6E 74 8B CD C7 06"))
            {
                static auto MessageBoxCtorHook = safetyhook::create_mid(pMessageBoxCtor, [](SafetyHookContext& regs)
                {
                    auto pBox = reinterpret_cast<uint8_t*>(regs.ecx);
                    if (pBox && std::find(sLiveMessageBoxes.begin(), sLiveMessageBoxes.end(), pBox) == sLiveMessageBoxes.end())
                        sLiveMessageBoxes.push_back(pBox);
                });
            }

            // CGameMessageBox::~CGameMessageBox, at the first instruction, before it puts the base
            // vftable back over whatever the object was.
            if (auto* pMessageBoxDtor = dunia_find("83 EC 18 56 8B F1 8D 8E 04 01 00 00 C7 06 ? ? ? ? C7 46 04 ? ? ? ? C7 46 70 ? ? ? ? E8"))
            {
                static auto MessageBoxDtorHook = safetyhook::create_mid(pMessageBoxDtor, [](SafetyHookContext& regs)
                {
                    auto pBox = reinterpret_cast<uint8_t*>(regs.ecx);
                    sLiveMessageBoxes.erase(std::remove(sLiveMessageBoxes.begin(), sLiveMessageBoxes.end(), pBox),
                        sLiveMessageBoxes.end());

                    // The draw is what puts the mask back, and the last box going away can be the
                    // last thing to happen before the menu stops drawing at all.
                    if (sLiveMessageBoxes.empty())
                        SetPadButtonMask(nNoPadButtonMask);
                });
            }
        };
    }
} ControllerPrompts;
