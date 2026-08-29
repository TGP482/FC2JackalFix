module;

#include <common.hxx>
#include <cstdint>
#include <vector>

export module controllerprompts;

import common;
import dunia;
import hudfixes;
import inputdevice;
import settings;

// Read at each use site: the nav bar is walked once a frame, the HUD once per prompt update.
static bool bControllerPrompts = true;

static bool PadPromptsWanted()
{
    return bControllerPrompts && IsPadActiveDevice();
}

// magma object layout

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

static ptrdiff_t nImageVtableRva = 0xEE6A04;
static ptrdiff_t nAreaInstanceVtableRva = 0xEE6BB4;
static ptrdiff_t nTextVtableRva = 0xEE63E4;
static ptrdiff_t nPlaceholderVtableRva = 0xEE9EA4;

// magma::ListBox and magma::RectShape; the shop page is built out of both.
static ptrdiff_t nListBoxVtableRva = 0xEE4794;
static ptrdiff_t nRectShapeVtableRva = 0xEE796C;

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

// 0xFFFFFFFF passes a null check and +0x08 off it wraps to 7. Arithmetic rather than IsReadable:
// these recurse over whole subtrees from a per-frame refresh.
static bool IsPlausiblePointer(const void* pAddress)
{
    const auto n = reinterpret_cast<uintptr_t>(pAddress);

    // Above the null page, below the end of the user half, aligned.
    return n >= 0x10000 && n < 0x7FFF0000 && (n & 3) == 0;
}

// Node as well as drawable: visibility lives on the Node. The 64 cap is sanity, not a format limit.
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

template <typename F>
static void ForEachChild(uint8_t* pArea, F&& fn)
{
    ForEachChildNode(pArea, [&](uint32_t nId, uint8_t*, uint8_t* pDrawable) { fn(nId, pDrawable); });
}

// Only these carry a magma::State at +0x08; reading a rect off anything else crashes.
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

// An AreaInstance carries its subtree on its own child Area, so descending takes one more hop.
static uint8_t* GetSubArea(uint8_t* pDrawable)
{
    if (!pDrawable || *reinterpret_cast<uintptr_t*>(pDrawable) != Rva(nAreaInstanceVtableRva))
        return nullptr;

    return *reinterpret_cast<uint8_t**>(pDrawable + nWidgetChildArea);
}

// Reports the Area the match was found in too: the HUD hangs its glyph off the icon's siblings.
static uint8_t* FindNamedDeep(uint8_t* pArea, uint32_t nWanted, int nMaxDepth = 4, uint8_t** ppParent = nullptr,
    int nDepth = 0)
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
            pFound = FindNamedDeep(pSub, nWanted, nMaxDepth, ppParent, nDepth + 1);
        }
    });

    return pFound;
}

// Direct children only, and the Node rather than the drawable.
static uint8_t* FindNamedNode(uint8_t* pArea, uint32_t nWanted)
{
    uint8_t* pFound = nullptr;

    ForEachChildNode(pArea, [&](uint32_t nId, uint8_t* pNode, uint8_t*)
    {
        if (!pFound && nId == nWanted)
            pFound = pNode;
    });

    return pFound;
}

// Building the widget the PC assets are missing

/*
  The PC prompt has no glyph child, so magma's factory builds one:

      Factory::CreateElementForType(ti)  0xABF0E0  Element plus drawable with a constructed State
      Factory::CreateKeyframe(ti)        0xABEE80
      Element::AddKeyframe(kf)           0xAB1C10
      Area::AddElement(e)                vtable +0x48
      Area::SetTime(t,0,0)               0xA973E0  forces one evaluation

  Must come from magma's pool, the Area destructor frees it. Keyframe not optional: an empty vector
  gets index 0xFFF and the draw gate refuses it. SetTime is seek and recurses, so seek back to the
  Area's own time or shared timelines jump.
*/
static ptrdiff_t nFactoryGlobalRva = 0x1664768;
static ptrdiff_t nImageTypeInfoRva = 0x1663F0C;
static ptrdiff_t nCreateElementRva = 0xABF0E0;
static ptrdiff_t nCreateKeyframeRva = 0xABEE80;
static ptrdiff_t nAddKeyframeRva = 0xAB1C10;
static ptrdiff_t nAreaSetTimeRva = 0xA973E0;

// Where SetTime records the time it was given.
static constexpr ptrdiff_t nAreaTime = 0x4C;

static constexpr ptrdiff_t nElementName = 0x08;
static constexpr ptrdiff_t nElementDrawable = 0x14;
static constexpr ptrdiff_t nKeyframeTime = 0x18;
static constexpr ptrdiff_t nAreaAddElementSlot = 0x48;

using CreateElement_t = uint8_t*(__thiscall*)(void*, void*);
using CreateKeyframe_t = uint8_t*(__thiscall*)(void*, void*);
using AddKeyframe_t = void(__thiscall*)(uint8_t*, uint8_t*);
using AreaAddElement_t = void(__thiscall*)(uint8_t*, uint8_t*);
using AreaSetTime_t = void(__thiscall*)(uint8_t*, int32_t, int32_t, int32_t);

// Not cached: a recycled Area address would hand back a freed widget.
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

    auto nTime = *reinterpret_cast<int32_t*>(pArea + nAreaTime);

    reinterpret_cast<AreaAddElement_t>(ppVtable[nAreaAddElementSlot / sizeof(void*)])(pArea, pElement);
    reinterpret_cast<AreaSetTime_t>(Rva(nAreaSetTimeRva))(pArea, nTime, 0, 0);

    return pDrawable;
}

// Menu nav bar

// FUN_105362E0. One stack argument, RET 4, so __stdcall.
using GetPadButtonImage_t = uintptr_t(__stdcall*)(int32_t);
static GetPadButtonImage_t GetPadButtonImage = nullptr;

// The suffix FUN_101D26D0's icon copy is pointed at, in place of "_pc": the data ships only
// icon_xenon. The show and text copies keep "_pc", show_pc having 140 real uses.
static const char szXenonSuffix[] = "_xenon";

// FUN_10533F10's names in id order; an icon value is matched here and resolved by FUN_105362E0.
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

// CNavBarPrompt, 0x54 bytes. +0x4C is the button id, 0x5C when given an icon value instead.
static constexpr ptrdiff_t nPromptNode = 0x04;
static constexpr ptrdiff_t nPromptElement = 0x08;
static ptrdiff_t nPromptSprite = 0x40;
static ptrdiff_t nPromptButtonId = 0x4C;
static ptrdiff_t nPromptAttached = 0x51;
static size_t nPromptSize = 0x54;
static ptrdiff_t nPromptVtableRva = 0xE25054;

// Xbox button ids as FUN_10533F10 numbers them.
static constexpr int32_t nPadA = 0;
static constexpr int32_t nPadB = 1;
static constexpr int32_t nPadX = 2;
static constexpr int32_t nPadY = 3;
static constexpr int32_t nPadRT = 5;
static constexpr int32_t nPadLB = 6;
static constexpr int32_t nPadRB = 7;

// magma::Node. FUN_10AB1A10 gates drawing and child recursion on bit 0.
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

// FUN_10AD9400 copies +0x30..+0x59 back wholesale, so the lock must cover everything, not the rect.
static constexpr uint32_t nAllComponentsLocked = 0xFFFFFFFF;

// magma::Text. Alignment is at +0x34, 0 left, 1 centre, 2 right, 3 justify.
static constexpr ptrdiff_t nTextString = 0x18;
static constexpr ptrdiff_t nTextAlignX = 0x34;
static constexpr int32_t nAlignCentre = 1;
static constexpr int32_t nAlignRight = 2;

// magma::TextState's shadow: a second draw from +0x44 whenever either signed offset is nonzero.
static constexpr ptrdiff_t nTextStateShadowColour = 0x44;
static constexpr ptrdiff_t nTextStateShadowOffsetX = 0x48;
static constexpr ptrdiff_t nTextStateShadowOffsetY = 0x49;

// MSVC std::string and std::wstring: the 16 byte buffer doubles as the pointer once capacity fills it.
static constexpr ptrdiff_t nStringBuffer = 0x04;
static constexpr ptrdiff_t nStringSize = 0x14;
static constexpr ptrdiff_t nStringCapacity = 0x18;
static constexpr uint32_t nStringLocalCapacity = 16;
static constexpr uint32_t nWideLocalCapacity = 8;

// A .mgb.desc attribute value. Anything longer is a bad read rather than a long value.
static constexpr uint32_t nStringSizeLimit = 0x400;

// Console draws the glyph 30x30 at 720p; in rect units that is 36/11 of the label's point size.
// All 33 glyph textures are 64x64, so one square suits every button.
static constexpr ptrdiff_t nTextStatePointSize = 0x40;
static constexpr int nMenuGlyphSizeNumerator = 36;
static constexpr int nMenuGlyphSizeDenominator = 11;
static constexpr int nMenuGlyphPixels = 30;

// The one length in rect units whose on-screen size is known; the HUD borrows it.
static int32_t nCalibratedGlyphSide = 0;

// If the point size cannot be read: 36/11 of the counted build's 150 unit fallback.
static constexpr int nMenuGlyphSideFallback = 409;

// Beside the label: 7 px counted on a 720p console capture, held against the disc so scale divides out.
static constexpr int nMenuGlyphGapPixels = 6;
static constexpr int nMenuGlyphRoundPixels = 31;

// The sprite carries a transparent margin, so the visible gap starts a margin further in.
static constexpr int nMenuGlyphQuadPixels = 65;
static constexpr int nMenuGlyphDiscPixels = 34;

// Unit space is square at 16:9, so widths are corrected for other aspects, height untouched.
// The category arrows do not take it, that quad being a pill counted whole.
static constexpr int nUiSpaceWide = 16;
static constexpr int nUiSpaceHigh = 9;

// FUN_10AB4F50. The State argument may be null, meaning the widget's own.
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

// "UI\360.mgb;360_a" -> 0. Document half dropped: every shipped value names 360.mgb.
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

// Pooled widgets arrive carrying the last tenant's flags and UVs.
static void ResetImageState(uint8_t* pState)
{
    *reinterpret_cast<float*>(pState + nStateRotation) = 0.0f;
    *reinterpret_cast<int16_t*>(pState + nStatePivotX) = 0;
    *reinterpret_cast<int16_t*>(pState + nStatePivotY) = 0;

    *reinterpret_cast<float*>(pState + nStateUvOffsetU) = 0.0f;
    *reinterpret_cast<float*>(pState + nStateUvOffsetV) = 0.0f;
    *reinterpret_cast<float*>(pState + nStateUvTilingU) = 1.0f;
    *reinterpret_cast<float*>(pState + nStateUvTilingV) = 1.0f;

    // Clears ACTUALSIZE, so the rect alone sizes the image, along with FLIPHORIZONTAL and FLIPVERTICAL.
    *reinterpret_cast<uint8_t*>(pState + nStateImageFlags) = 0;

    *reinterpret_cast<uint32_t*>(pState + nStateShadowColour) = 0;
    *reinterpret_cast<int8_t*>(pState + nStateShadowOffsetX) = 0;
    *reinterpret_cast<int8_t*>(pState + nStateShadowOffsetY) = 0;

    for (int i = 0; i < nColourCount; ++i)
        *reinterpret_cast<uint32_t*>(pState + nStateColour + i * 4) = nOpaqueWhite;
}

// FUN_10AB4F50 returns the string's start in the rect; under right alignment that is box width
// less text width. The alignment field is a plain member, so forcing it for one call is safe.
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

// The button picks the side, not the slot: "(B) Cancel" against "Accept (A)". Slot only where
// there is no button.
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

// Split out of the placement so it runs on either device: otherwise the shop sizes off nothing.
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

// The shape the engine composes into, shared with the HUD: the window is not it once an internal
// resolution is in force, and enumerating for it caught whatever overlay was topmost in the process.
static int32_t GlyphWidth(int32_t nSide)
{
    float fAspect = 0.0f;
    if (!GetFrameAspect(fAspect) || fAspect <= 0.0f)
        return nSide;

    // One division, so 16:9 lands back on nSide exactly.
    auto nWide = static_cast<int32_t>(
        nSide * (static_cast<float>(nUiSpaceWide) / nUiSpaceHigh) / fAspect + 0.5f);

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

    // No point size to calibrate against, so the constant stands in and is recorded.
    if (nSide < 1)
    {
        nSide = nMenuGlyphSideFallback;
        nCalibratedGlyphSide = nSide;
    }

    auto nWide = GlyphWidth(nSide);

    // Both off the display-corrected width, so proportions hold.
    auto nDisc = nWide * nMenuGlyphDiscPixels / nMenuGlyphQuadPixels;
    auto nMargin = (nWide - nDisc) / 2;
    auto nGap = nDisc * nMenuGlyphGapPixels / nMenuGlyphRoundPixels - nMargin;

    auto nCentreY = (box.nTop + box.nBottom) / 2;

    out.nTop = static_cast<int16_t>(nCentreY - nSide / 2);
    out.nBottom = static_cast<int16_t>(out.nTop + nSide);

    // Box edge only as fallback, the margin having been solved against the text. A zero width must
    // not be taken: a centred empty label puts both edges where the text will land.
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

// Shadow under both halves of a menu prompt, in 720p pixels via the calibrated square.
// Provisional, neither number counted: 0xAABBGGRR, half alpha, opaque black drew as a second copy.
static constexpr uint32_t nPromptShadowColour = 0x80000000;
static constexpr int nPromptShadowPixels = 1;

// int8 rect units added straight to the rect corners, so the horizontal one goes through GlyphWidth.
static void WriteShadow(uint8_t* pState, ptrdiff_t nColour, ptrdiff_t nOffsetX, ptrdiff_t nOffsetY,
    bool bWanted)
{
    auto nSide = nCalibratedGlyphSide;
    auto bOn = bWanted && nSide > 0;

    auto nDown = bOn ? nSide * nPromptShadowPixels / nMenuGlyphPixels : 0;
    auto nAcross = bOn ? GlyphWidth(nSide) * nPromptShadowPixels / nMenuGlyphPixels : 0;

    *reinterpret_cast<uint32_t*>(pState + nColour) = bOn ? nPromptShadowColour : 0;
    *reinterpret_cast<int8_t*>(pState + nOffsetX) = static_cast<int8_t>(std::clamp(nAcross, 0, 127));
    *reinterpret_cast<int8_t*>(pState + nOffsetY) = static_cast<int8_t>(std::clamp(nDown, 0, 127));
}

// The label is the engine's widget with no component lock, so its shadow is rewritten every frame.
static void ShadowLabel(uint8_t* pArea, bool bWanted)
{
    static const auto nLabelName = NameId("t_button_text");

    if (auto pState = GetState(FindNamedDeep(pArea, nLabelName, 0)))
        WriteShadow(pState, nTextStateShadowColour, nTextStateShadowOffsetX, nTextStateShadowOffsetY,
            bWanted);
}

// Runs at SetIcon in CNavBarPrompt::OnAttach, and for every attached prompt on a device flip.
static bool IsReadable(const void* pAddress);
static bool IsObject(uint8_t* pObject);
static bool ComputeBazaarGlyphRect(uint8_t* pLabel, Rect& out);
static void TintBazaarGlyph(uint8_t* pState);
static const char* ShopLetterFromButton(int32_t nButton);
static uintptr_t GetShopSprite(const char* szLetter);

// Whether the computer's message boxes wear its glyphs; driven off the page enter and leave hooks,
// nothing on a bazaar button saying which page it belongs to.
static constexpr int nShopGlyphToggleKey = VK_F10;
static bool bShopGlyphsOnPrompts = false;

static void ApplyMenuGlyph(uint8_t* pPrompt)
{
    // An attached prompt can still hold a stale element: a crash faulted on 0x5D through a null check.
    auto pElement = *reinterpret_cast<uint8_t**>(pPrompt + nPromptElement);
    if (!IsObject(pElement))
        return;

    auto pArea = *reinterpret_cast<uint8_t**>(pElement + nWidgetChildArea);
    if (!IsReadable(pArea))
        return;

    // Ahead of the device branch below, which returns before reaching the placement.
    CalibrateGlyphSide(pArea);

    // The PC art pass deleted "i_placeholder", and FUN_10189BA0 would refill it on the next attach.
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

        // Every prompt that draws ships with its pill visible.
        SetNodeVisible(pBackground, true);
        ShadowLabel(pArea, false);
        return;
    }

    if (!pImage)
        pImage = BuildImageChild(pArea, nGlyphName);

    auto pState = GetState(pImage);
    if (!pState)
        return;

    // The menu's square is wrong on the computer's page: larger font, twice the scale.
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

    WriteShadow(pState, nStateShadowColour, nStateShadowOffsetX, nStateShadowOffsetY, true);
    ShadowLabel(pArea, true);

    *reinterpret_cast<uint32_t*>(pImage + nWidgetLockMask) = nAllComponentsLocked;
    *reinterpret_cast<uintptr_t*>(pImage + nImageSprite) = nSprite;

    // 360 ships no pill behind the glyph.
    SetNodeVisible(pBackground, false);
}

// Catching up mid page

// CNavBarModule only touches a prompt on attach, so the sets -- not the prompts, which a set holds
// by value and moves when it grows -- are kept and re-walked on a device change.
static constexpr ptrdiff_t nPromptSetArray = 0x04;
static constexpr ptrdiff_t nPromptSetCount = 0x08;

// The nav bars carry four or five; a wild count means a freed set.
static constexpr uint32_t nPromptSetMax = 16;

// A vector: attach order is the only thing separating a modal's prompts from the page's.
static std::vector<uint8_t*> sPromptSets;

// VirtualQuery rather than an SEH frame: same cost, and the walk stays an ordinary function.
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

// Comfortably past Dunia's mapped size, so a vtable check is a range test, not an exact one.
static constexpr uintptr_t nDuniaImageSize = 0x2000000;

// Readable is not real: every vtable in this tree lives in Dunia, so one read rejects a stray integer.
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

// Newest set first, newest prompt within it; returning true stops the walk.
template <typename Fn>
static void ForEachLivePrompt(Fn&& fn)
{
    for (auto it = sPromptSets.rbegin(); it != sPromptSets.rend(); ++it)
    {
        auto pSet = *it;
        if (!IsObject(pSet))
            continue;

        auto pPrompts = *reinterpret_cast<uint8_t**>(pSet + nPromptSetArray);
        auto nCount = *reinterpret_cast<uint32_t*>(pSet + nPromptSetCount);
        if (!pPrompts || nCount > nPromptSetMax)
            continue;

        for (auto i = static_cast<int32_t>(nCount) - 1; i >= 0; i--)
        {
            auto pPrompt = pPrompts + i * static_cast<ptrdiff_t>(nPromptSize);
            if (IsLivePrompt(pPrompt) && fn(pPrompt))
                return;
        }
    }
}

// Moved to the back so the set that attached most recently is the one the pad buttons ask first.
static void RememberPromptSet(uint8_t* pSet)
{
    if (!pSet)
        return;

    sPromptSets.erase(std::remove(sPromptSets.begin(), sPromptSets.end(), pSet), sPromptSets.end());
    sPromptSets.push_back(pSet);
}

static void RefreshMenuGlyphs()
{
    ForEachLivePrompt([](uint8_t* pPrompt)
    {
        ApplyMenuGlyph(pPrompt);
        return false;
    });
}

// Gameplay HUD

// Built as an Image here too: a HUD prompt subtree holds no magma::Text. Buttons from the shipped
// console action map: use -> a and y, reload -> x, tryuseied -> RT, heal -> LB.

// CHudPrompt, stride 0x60. +0x08 is the show request, +0x0C the Area <Prompt path="..."> resolved to.
static constexpr ptrdiff_t nHudPromptName = 0x04;
static constexpr ptrdiff_t nHudPromptShown = 0x08;
static constexpr ptrdiff_t nHudPromptArea = 0x0C;

// The call inside CHudPromptMgr::Update's per prompt loop; hands the prompt over in ECX.
static constexpr ptrdiff_t nHudPromptUpdateCall = 0x0E;

// Square in 720p pixels via the calibrated side; before any menu prompt, a share of the icon beneath.
static constexpr int nHudGlyphGapPercent = 20;
static constexpr int nHudGlyphFallbackPercent = 85;

// Centred on the anchor rect, not the screen, so it lands 2-3 px right of console.

// Offset does not convert at the same rate as size, cause unknown: 17 px requested landed at 33,
// 12 at 23, slope two. The square itself is within a pixel or two of Xenia at both sizes.
static constexpr int nHudOffsetHalving = 2;

// szAbove is both the sibling the glyph is added beside and the rect it is measured from.
// nDown counted off Xenia; the inventory prompts have no capture and take interact's.
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

// Re-run every update: the icon animates in, so a first-frame square would be pinned far too small.
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

    // Centred on the anchor, so the narrower width costs only the stretch.
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
    auto pAnchor = FindNamedDeep(pArea, NameId(prompt.szAbove), 8, &pSiblingArea);
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

// The mouse pointer

// Only the enabled bitmask: FUN_10AB75C0(mgr, slot, enabled) is one bit of +0x29 and fires the
// leave first. The wider FUN_104EEB20 cost pad navigation and vibration.
static ptrdiff_t nScreenManagerRva = 0xFE3178;
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

// FUN_104F0FB0(this, event), the UI input dispatcher: per event, so it beats the cursor re-enable
// on a screen transition. Event words: device name hash, control hash, type.
// Triggers are dropped whole here: both edges arrive as virtual key 0 and cannot pair, so the down
// takes the key capture and nothing releases it. Editing the event took the trigger off the gun.
static constexpr uint32_t nPadDeviceName = 0x9D894EE5;
static constexpr uint32_t nRightTriggerName = 0x7DB6BD7B;
static constexpr uint32_t nLeftTriggerName = 0x0DB90A3B;

static SafetyHookInline UiInputHook{};

static char __fastcall UiInput(void* pThis, void* pEdx, uint32_t* pEvent)
{
    // The dispatcher sees key events, so the press that flips this also applies it.
    static auto bToggleWasDown = false;
    auto bToggleDown = (GetAsyncKeyState(nShopGlyphToggleKey) & 0x8000) != 0;

    if (bToggleDown && !bToggleWasDown)
    {
        bShopGlyphsOnPrompts = !bShopGlyphsOnPrompts;
        RefreshMenuGlyphs();
    }

    bToggleWasDown = bToggleDown;

    // FUN_10AB75C0 reaches back into the event machinery to fire the leave.
    if (!bInCursorUpdate)
    {
        bInCursorUpdate = true;

        if (PadPromptsWanted())
            DisableCursors();
        else
            RestoreCursors();

        bInCursorUpdate = false;
    }

    if (IsReadable(pEvent) && pEvent[0] == nPadDeviceName
        && (pEvent[1] == nRightTriggerName || pEvent[1] == nLeftTriggerName))
    {
        // Unhandled, so every other sink is left alone.
        return 0;
    }

    return UiInputHook.fastcall<char>(pThis, pEdx, pEvent);
}

// The shop computer

// The green outline art ships on PC in ui/360.mgb, 360_a_shop beside the gold set; sprite names
// follow texture names. The document is cached off FUN_105362E0's own load, a hand built
// std::string having crashed in the game's msvcr80. Glyph sits left of the text bar, which stays.
// Buttons are found by name, the UI's cached label pointers not being the widgets on screen.
static constexpr size_t nSpriteNameSize = 64;

// Offsets from this module's anchor in FUN_105362E0 to the three name-to-sprite calls, each E8 rel32.
static constexpr ptrdiff_t nMakeSpriteNameCall = 0x1D;
static constexpr ptrdiff_t nResolveSpriteCall = 0x29;
static constexpr ptrdiff_t nFreeSpriteNameCall = 0x34;

// Just past CALL FUN_105355B0, EAX the ui/360.mgb document; behind the anchor, not ahead.
static constexpr ptrdiff_t nUiDocumentLoaded = -0x3A;

// Entry of CBazaarComputerUI::RefreshFocus to the MOV EDI,ECX, so the UI is in EDI.
static constexpr ptrdiff_t nBazaarRefreshThis = 0x09;

// The two calls inside it that turn an element name into a drawable.
static constexpr ptrdiff_t nMakeElementNameCall = 0x55;
static constexpr ptrdiff_t nFindElementCall = 0x60;
static ptrdiff_t nUiManagerRva = 0x1645C3C;

static constexpr uint32_t nBazaarLabelId = 0x81FDCEB5;

// The computer's green, 0xAABBGGRR, written out rather than sampled: a label's colour dims with
// focus and the tint multiplies.
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

// FUN_10720880: name and lookup in one, handing back the Element rather than what it draws.
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

// Sprite names follow texture names, so several spellings are tried with gold behind them all.
static constexpr std::array<const char*, 2> sShopSuffixes = {{ "_shop", "" }};
static constexpr std::array<const char*, 2> sShopPrefixes = {{ "button_", "360_" }};

// FUN_107330C0 resolves from +0x1A8: three shop names, then CANCEL_CHECKOUT, ADDREMOVE_CHECKOUT
// and CHECKOUT_CHECKOUT at +0x1B4..+0x1BC, the last two with no WEAPON_ twin.
static constexpr std::array<BazaarButton, 6> sBazaarButtons =
{{
    { "WEAPON_BAZAAR_BUTTON_CANCEL",          "b", nPadB },
    { "WEAPON_BAZAAR_BUTTON_ADDREMOVE",       "a", nPadA },
    { "WEAPON_BAZAAR_BUTTON_CHECKOUT",        "y", nPadY },
    { "WEAPON_BAZAAR_BUTTON_CANCEL_CHECKOUT", "b", nPadB },
    { "BAZAAR_BUTTON_ADDREMOVE_CHECKOUT",     "a", nPadA },
    { "BAZAAR_BUTTON_CHECKOUT_CHECKOUT",      "y", nPadY },
}};

// Undrawn templates read back boxes like 0..0 by -24236..4334, so anything far outside is rejected.
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

// +0x28 and +0x2C are whatever sits there on a non-Area, so the pair is checked before walking.
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

// Most widgets hang an Area off +0x3C; magma::Button at 0xEE800C is one itself.
static uint8_t* AsChildArea(uint8_t* pDrawable)
{
    if (!pDrawable)
        return nullptr;

    auto pArea = *reinterpret_cast<uint8_t**>(pDrawable + nWidgetChildArea);
    if (LooksLikeArea(pArea))
        return pArea;

    return LooksLikeArea(pDrawable) ? pDrawable : nullptr;
}

// By drawable, not name: a Node's id is the slot it fills, not the name asked for.
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

// FUN_10730610's second type check against 0xEE7E90 rejects the checkout page's add/remove and
// checkout; FUN_10720880, the same lookup without it, is the fallback below.
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

// The _CHECKOUT pair resolve to parts of a button, so the holding Area carries label and glyph.
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

    // One object, built and handed to the document exactly as FUN_105362E0 does.
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

    // The bazaar draws at twice the menu's scale: the calibrated square measured 63 px here.
    auto nBase = nCalibratedGlyphSide > 0 ? nCalibratedGlyphSide : nMenuGlyphSideFallback;

    auto nSide = nBase * nBazaarGlyphPixels / nBazaarGlyphMeasured;
    if (nSide < 1)
        return false;

    // From the button's left edge, not the label box: Checkout's box starts at 33 behind the cart
    // sprite. A negative left is fine, the area does not clip.
    auto nGap = nBase * nBazaarGapPixels / nBazaarGlyphMeasured;
    auto nCentreY = (box.nTop + box.nBottom) / 2;

    // Hung off the bar's edge, so the narrower width takes the glyph in from the left.
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

    // Nothing of the game's is hidden either way: the bar stays on both devices.
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

// The "<<" and ">>" either side of the WEAPONS bar: members of the list, so reached off
// magma::ListBox rather than by name.

// The pill covers a little under three fifths of its texture, so console's 37x20 wants 63x33.
static constexpr int nBazaarArrowQuadWide = 63;
static constexpr int nBazaarArrowQuadHigh = 33;
static constexpr int nBazaarArrowWidePixels = 37;

static constexpr int nBazaarArrowInsetPixels =
    (nBazaarArrowQuadWide - nBazaarArrowWidePixels) / 2;

// Measured against Xenia: on the inner edge the pill cleared the bar 5 left, 6 right against
// console's 9, and sat 2 px high.
static constexpr int nBazaarArrowNudgePixels = 4;
static constexpr int nBazaarArrowDropPixels = 2;

// Arrows at +0x58 and +0x74, each drawn under a translate the list derives from the arrow's own
// width, so the pill anchors on the arrow's box and not on the bar.
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

// Console draws no box there, only the pill, so the "<<" and its border come off with a pad in hand.
static void ShowArrowContent(uint8_t* pArea, uint32_t nGlyphName, bool bVisible)
{
    ForEachChildNode(pArea, [&](uint32_t nId, uint8_t* pNode, uint8_t*)
    {
        if (nId != nGlyphName)
            SetNodeVisible(pNode, bVisible);
    });
}

// Union of the bordered box and the "<<": a Button carries nothing at +0x08 to read.
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

    // Pill on the box's inner edge, the empty texture beside it over the bar.
    Rect rect;

    // The nudge stays inside the box's own span, so the list's layout width does not change.
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

// For a device change: RefreshFocus places these otherwise and does not run then. Gated on the
// page being open, the bazaar elements resolving from anywhere.
static void ApplyBazaarGlyphs()
{
    for (const auto& button : sBazaarButtons)
        ApplyBazaarGlyph(button);

    ApplyBazaarArrows();
}

static void RefreshBazaarGlyphs()
{
    if (bShopGlyphsOnPrompts)
        ApplyBazaarGlyphs();
}

// Menu buttons the PC build left on the mouse

/*
  Default, Apply, Create and Edit have a prompt and a glyph and no pad input: A and B arrive as
  Enter and Escape, X and Y as UI keys 0x202 and 0x203 nothing in Dunia compares against. So a
  press is synthesised through the pointer path, dispatched through the hovered Node's vtable+0x8C,
  that Node being CNavBarPrompt+0x04; its widget at +0x08 crashes.

      manager + 0x04 / + 0x08   page stack, begin and end, 8 bytes a slot, page first
      manager + 0x28 / + 0x29   device and cursor bitmasks, both refused by the handlers when clear
      manager + 0x2C            pointer position per device, 8 bytes a slot, written by a press
      page    + 0x28 / + 0x2C   its own child vector, a Page being an Area
      page    + 100 + d * 0x3C  per device: + 0x1C hovered Node, + 0x20 captured

  A and B are taken over only on a message box, and cleared from the driver's XINPUT_STATE while
  one is open so the action map cannot close it twice.
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

// Event words: button, packed position, device byte, dispatcher flag.
static constexpr size_t nMouseEventWords = 4;

static constexpr ptrdiff_t nStateOriginX = 0x24;
static constexpr ptrdiff_t nStateOriginY = 0x26;

static constexpr uint16_t nXInputA = 0x1000;
static constexpr uint16_t nXInputB = 0x2000;
static constexpr uint16_t nXInputX = 0x4000;
static constexpr uint16_t nXInputY = 0x8000;

static constexpr uint16_t nNoPadButtonMask = 0xFFFF;

using UiMouseEvent_t = char(__thiscall*)(void*, uint32_t*);
static UiMouseEvent_t UiMouseMove = nullptr;
static UiMouseEvent_t UiMouseDown = nullptr;
static UiMouseEvent_t UiMouseUp = nullptr;

// Every CGameMessageBox subclass counts; the live object does not carry the base vftable.
static std::vector<uint8_t*> sLiveMessageBoxes;

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

// Whether a page holds the Node, and how far in: FUN_10AA23E0 subtracts each child's rect on the
// way down, so the walk adds up what it descended through.
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

        // +0x3C is whatever sits there on classes with no child Area, so both ends are checked.
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

// A press can take the page down, so saved state only goes back on a page still on the stack.
static bool PageOnStack(uint8_t* pManager, uint8_t* pPage)
{
    auto pBegin = *reinterpret_cast<uint8_t**>(pManager + nScreenStackBegin);
    auto pEnd = *reinterpret_cast<uint8_t**>(pManager + nScreenStackEnd);

    for (auto* pSlot = pBegin; pBegin && pEnd && pSlot + nScreenStackStride <= pEnd; pSlot += nScreenStackStride)
    {
        if (*reinterpret_cast<uint8_t**>(pSlot) == pPage)
            return true;
    }

    return false;
}

static uint8_t** DeviceSlot(uint8_t* pScreen, ptrdiff_t nSlot)
{
    return reinterpret_cast<uint8_t**>(
        pScreen + nScreenDeviceBase + nMenuPadDevice * nScreenDeviceStride + nSlot);
}

// The stack grows upwards, so the last slot is what the player is looking at.
static uint8_t* TopScreen()
{
    auto pManager = ScreenManager();
    if (!pManager)
        return nullptr;

    auto pBegin = *reinterpret_cast<uint8_t**>(pManager + nScreenStackBegin);
    auto pEnd = *reinterpret_cast<uint8_t**>(pManager + nScreenStackEnd);
    if (!pBegin || !pEnd || pEnd <= pBegin)
        return nullptr;

    auto pTop = *reinterpret_cast<uint8_t**>(pEnd - nScreenStackStride);

    return IsObject(pTop) ? pTop : nullptr;
}

// A box hangs its page on the top screen's layer, so neither the stack nor attach order tells its
// prompts apart; its own CNavBarModule at box+0x104 does, prompt array off +0x0C.
static constexpr ptrdiff_t nBoxNavBarModule = 0x104;
static constexpr ptrdiff_t nNavBarLayout = 0x0C;
static constexpr ptrdiff_t nNavBarPromptArray = 0x04;
static constexpr ptrdiff_t nNavBarPromptCount = 0x08;
static constexpr size_t nNavBarPromptStride = 0x54;

static bool PromptFromBox(uint8_t* pPrompt)
{
    for (auto* pBox : sLiveMessageBoxes)
    {
        if (!pPrompt || !IsReadable(pBox) || !IsReadable(pBox + nBoxNavBarModule))
            continue;

        auto pLayout = *reinterpret_cast<uint8_t**>(pBox + nBoxNavBarModule + nNavBarLayout);
        if (!IsReadable(pLayout))
            continue;

        auto pArray = *reinterpret_cast<uint8_t**>(pLayout + nNavBarPromptArray);
        auto nCount = *reinterpret_cast<uint32_t*>(pLayout + nNavBarPromptCount);
        if (!IsReadable(pArray) || pPrompt < pArray)
            continue;

        auto nOffset = static_cast<size_t>(pPrompt - pArray);
        if (nOffset % nNavBarPromptStride == 0 && nOffset / nNavBarPromptStride < nCount)
            return true;
    }

    return false;
}

// CNavBarPrompt::Activate, __fastcall on the prompt, fires its delegate and says whether it fired.
// A box's delegate needs no pointer position, so this is tried before the synthetic press.
using ActivatePrompt_t = int(__fastcall*)(void*);
static ActivatePrompt_t ActivatePrompt = nullptr;

// Backwards, so a modal's prompts win. Node and widget checked as a pair.
static uint8_t* FindPrompt(int32_t nButton)
{
    uint8_t* pFound = nullptr;

    ForEachLivePrompt([&](uint8_t* pPrompt)
    {
        if (*reinterpret_cast<int32_t*>(pPrompt + nPromptButtonId) != nButton)
            return false;

        auto pNode = *reinterpret_cast<uint8_t**>(pPrompt + nPromptNode);
        auto pWidget = *reinterpret_cast<uint8_t**>(pPrompt + nPromptElement);

        if (!IsObject(pNode) || !IsObject(pWidget)
            || *reinterpret_cast<uint8_t**>(pNode + nNodeDrawable) != pWidget)
        {
            return false;
        }

        pFound = pPrompt;
        return true;
    });

    return pFound;
}

static bool PressPrompt(uint8_t* pPrompt)
{
    auto bActed = false;

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

    // Handlers refuse a device that is not on. Written directly: FUN_10AB75C0 fires a leave.
    auto nBit = static_cast<uint8_t>(1 << nMenuPadDevice);
    *(pManager + nDeviceActiveMask) = static_cast<uint8_t>(nSavedActive | nBit);
    *(pManager + nCursorEnabledMask) = static_cast<uint8_t>(nSavedEnabled | nBit);

    // The probe goes to 0,0, off every page, so only the transform comes back.
    bCalibrated = false;
    pCalibrateScreen = pPage;

    uint32_t probe[nMouseEventWords] = { 0, 0, nMenuPadDevice, 0 };
    UiMouseMove(pManager, probe);

    pCalibrateScreen = nullptr;

    if (bCalibrated)
    {
        auto nPosition = PackPosition(
            nCalibrateX + nOffsetX + *reinterpret_cast<int16_t*>(pState + nStateOriginX) + (box.nLeft + box.nRight) / 2,
            nCalibrateY + nOffsetY + *reinterpret_cast<int16_t*>(pState + nStateOriginY) + (box.nTop + box.nBottom) / 2);

        uint32_t move[nMouseEventWords] = { 0, nPosition, nMenuPadDevice, 0 };
        UiMouseMove(pManager, move);

        auto ppHovered = DeviceSlot(pPage, nScreenHovered);
        auto ppCaptured = DeviceSlot(pPage, nScreenCaptured);

        auto bLanded = *ppHovered == pNode;
        auto pSavedHovered = *ppHovered;

        // The dispatcher stops at the first page that handles the press, and a page with nothing
        // hovered or captured handles nothing. Clearing above a landed press cost a second accept.
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

                *DeviceSlot(pScreen, nScreenHovered) = nullptr;
                *DeviceSlot(pScreen, nScreenCaptured) = nullptr;
            }

            *ppHovered = pNode;
        }

        // FUN_10AA5930 skips the press if +0x20 is taken; FUN_10AA2F80 releases what is in it.
        *ppCaptured = nullptr;

        uint32_t press[nMouseEventWords] = { nLeftMouseButton, nPosition, nMenuPadDevice, 1 };
        auto bDown = UiMouseDown(pManager, press) != 0;
        UiMouseUp(pManager, press);

        // A forced press that acted leaves nothing under the pointer; a landed one says so itself.
        auto bAlive = PageOnStack(pManager, pPage);
        bActed = !bAlive || (bLanded ? bDown : *ppHovered == nullptr);

        // The neighbour goes back only if nothing happened: an empty slot fires an enter next time,
        // and a press that acted can have freed the Node.
        if (!bLanded && bAlive && !bActed)
            *ppHovered = pSavedHovered;
    }

    *(pManager + nDeviceActiveMask) = nSavedActive;
    *(pManager + nCursorEnabledMask) = nSavedEnabled;
    *pCursor = nSavedCursor;

    return bActed;
}

struct MenuPadButton
{
    uint16_t nBit;
    int32_t nPromptButton;

    // A and B mean something already; see the section note.
    bool bMessageBoxOnly;
};

static constexpr std::array<MenuPadButton, 4> sMenuPadButtons =
{{
    { nXInputA, nPadA, true },
    { nXInputB, nPadB, true },
    { nXInputX, nPadX, false },
    { nXInputY, nPadY, false },
}};

// A press that did nothing is retried on the next draw, up to a few.
//
// ponytail: a retry on a read-back rather than knowing what the widget wants going in; if the
// pointer's enter is worth synthesising, this goes.
static constexpr int nPressRetries = 4;

static uint8_t* pPendingPrompt = nullptr;
static int nPendingTries = 0;

// Runs off the UI draw, already walking the attached prompts and only while a menu is up.
static void UpdateMenuPadButtons()
{
    static uint16_t nHeld = 0;

    if (!IsPadActiveDevice())
    {
        nHeld = 0;
        pPendingPrompt = nullptr;
        SetPadButtonMask(nNoPadButtonMask);
        return;
    }

    if (auto* pPending = pPendingPrompt)
    {
        pPendingPrompt = nullptr;

        if (++nPendingTries < nPressRetries && IsLivePrompt(pPending) && !PressPrompt(pPending))
            pPendingPrompt = pPending;
    }

    auto nButtons = GetPadState().nButtons;
    auto nPressed = static_cast<uint16_t>(nButtons & ~nHeld);

    auto bMessageBox = !sLiveMessageBoxes.empty();

    nHeld = nButtons;

    auto nMask = nNoPadButtonMask;

    for (const auto& button : sMenuPadButtons)
    {
        // A and B mean Enter and Escape outside a box, so taken over only while one is up. X and Y
        // mean nothing to the action map either way.
        if (button.bMessageBoxOnly && !bMessageBox)
            continue;

        auto pPrompt = FindPrompt(button.nPromptButton);
        if (!pPrompt || (bMessageBox && !PromptFromBox(pPrompt)))
            continue;

        // Held for as long as the prompt is there: the action map polls separately and would win.
        if (button.bMessageBoxOnly)
            nMask = static_cast<uint16_t>(nMask & ~button.nBit);

        if (nPressed & button.nBit)
        {
            nPendingTries = 0;

            auto bFired = ActivatePrompt && ActivatePrompt(pPrompt) != 0;
            pPendingPrompt = bFired || PressPrompt(pPrompt) ? nullptr : pPrompt;
        }
    }

    SetPadButtonMask(nMask);
}

// GOG is the same 1.03 source recompiled: code near identical, data moved, and CNavBarPrompt lost
// 0x18 bytes ahead of its string, dragging every member past +8 down.
static void ApplyBuildFixups()
{
    if (!IsGOG())
        return;

    nImageVtableRva = 0xE5E034;
    nAreaInstanceVtableRva = 0xE5E1E4;
    nTextVtableRva = 0xE5DA14;
    nPlaceholderVtableRva = 0xE614D4;
    nListBoxVtableRva = 0xE5BDC4;
    nRectShapeVtableRva = 0xE5EF9C;

    nFactoryGlobalRva = 0x15B3B10;
    nImageTypeInfoRva = 0x15B32B4;
    nCreateElementRva = 0xAAE7B0;
    nCreateKeyframeRva = 0xAAE550;
    nAddKeyframeRva = 0xAA12D0;
    nAreaSetTimeRva = 0xA860B0;

    nScreenManagerRva = 0xF32878;
    nUiManagerRva = 0x159501C;

    nPromptVtableRva = 0xD9EC84;
    nPromptSprite = 0x28;
    nPromptButtonId = 0x34;
    nPromptAttached = 0x39;
    nPromptSize = 0x3C;
}

class ControllerPrompts
{
public:
    ControllerPrompts()
    {
        JackalFix::onDuniaInitEvent().add(ApplyBuildFixups, nBuildFixupPriority);

        JackalFix::onDuniaInitEvent() += []()
        {
            bControllerPrompts = JackalFixSettings.GetInt(PREF_CONTROLLERPROMPTS) != 0;

            // Read where it is used: touching a widget tree on the file watcher's thread is not safe.
            JackalFix::onIniFileChange() += []()
            {
                bControllerPrompts = JackalFixSettings.GetInt(PREF_CONTROLLERPROMPTS) != 0;
            };

            // FUN_105362E0, the 360 glyph loader. Anchored on its two null checks, the prologue
            // being std::string boilerplate shared with unrelated functions.
            static constexpr ptrdiff_t nGlyphLoaderEntry = 0x96;
            auto glyphPattern = dunia_pattern("3B F3 74 3F 8B 44 24 48 8B 0D ? ? ? ? 50 E8 ? ? ? ? 3B C3 74 2B 50 8D 4C 24 2C E8");
            if (!glyphPattern.empty())
            {
                auto pMatch = reinterpret_cast<uint8_t*>(glyphPattern.get_first());
                GetPadButtonImage = reinterpret_cast<GetPadButtonImage_t>(pMatch - nGlyphLoaderEntry);

                // The name to sprite calls, off this function's own call sites.
                MakeSpriteName = reinterpret_cast<MakeSpriteName_t>(CallTarget(pMatch + nMakeSpriteNameCall));
                ResolveSprite = reinterpret_cast<ResolveSprite_t>(CallTarget(pMatch + nResolveSpriteCall));
                FreeSpriteName = reinterpret_cast<FreeSpriteName_t>(CallTarget(pMatch + nFreeSpriteNameCall));
            }

            // FUN_101D26D0's icon copy of "_pc", told from the show and text copies by the "icon"
            // push ahead of it; string pointer at +0x1E. GOG moved every stack displacement.
            static constexpr ptrdiff_t nIconSuffixOperand = 0x1E;
            auto iconSuffixPattern = dunia_pattern("8D 84 24 ? 00 00 00 50 8B CF E8 ? ? ? ? 68 ? ? ? ? 8D 4C 24 ? E8 ? ? ? ? 68 ? ? ? ? 8D 4C 24 ? E8 ? ? ? ? 6A FF 6A 00 8D 4C 24 ? 51 8D 4C 24 ?");
            if (!iconSuffixPattern.empty())
            {
                auto pSuffixOperand = iconSuffixPattern.get_first(nIconSuffixOperand);
                injector::WriteMemory<uintptr_t>(pSuffixOperand, reinterpret_cast<uintptr_t>(szXenonSuffix), true);
            }

            // Just past the engine's icon resolve, which sets the id to 0x5C either way; resolving
            // here keeps it. Value displacement off the LEA: 0xA4 Steam, 0x9C GOG.
            static constexpr ptrdiff_t nMenuIconApplied = 0x0F;
            static constexpr ptrdiff_t nMenuIconValueOperand = 0x03;
            static ptrdiff_t nMenuIconValue = 0xA4;
            auto menuIconPattern = dunia_pattern("8D 84 24 ? 00 00 00 50 8B CF E8 ? ? ? ? 8D 8C 24 ? 00 00 00 E8 ? ? ? ? 8B 16 8B 52 40");
            if (!menuIconPattern.empty() && GetPadButtonImage)
            {
                nMenuIconValue = *reinterpret_cast<uint32_t*>(menuIconPattern.get_first(nMenuIconValueOperand));

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

            // CNavBarPrompt::Activate, FUN_10189D40. GOG loads the owner at +0x30, not +0x48.
            if (auto* pActivatePrompt = dunia_find(
                "56 8B F1 8B 4E 04 85 C9 74 4F 8B 01 8B 15 ? ? ? ? 8B 40 08 52 FF D0 85 C0 74 3D 8B 10 8B C8 8B 42 44 FF D0 84 C0 74 30 8B 76 48 85 F6 74 29",
                "56 8B F1 8B 4E 04 85 C9 74 4F 8B 01 8B 15 ? ? ? ? 8B 40 08 52 FF D0 85 C0 74 3D 8B 10 8B C8 8B 42 44 FF D0 84 C0 74 30 8B 76 30 85 F6 74 29"))
                ActivatePrompt = reinterpret_cast<ActivatePrompt_t>(pActivatePrompt);

            // FUN_10AB4F50, magma::Text's alignment offset.
            static constexpr ptrdiff_t nTextAlignOffsetEntry = 0x27;
            auto textAlignPattern = dunia_pattern("8B F9 8B 74 24 54 85 F6 75 03 8B 77 08 8B 47 58 85 C0 75 07 33 C0 E9");
            if (!textAlignPattern.empty())
            {
                auto pMatch = reinterpret_cast<uint8_t*>(textAlignPattern.get_first());
                GetTextAlignOffset = reinterpret_cast<GetTextAlignOffset_t>(pMatch - nTextAlignOffsetEntry);
            }

            // CNavBarPrompt::OnAttach's tail call to SetIcon: element non null, sprite in hand,
            // ESI the prompt. GOG members 0x18 lower.
            static constexpr ptrdiff_t nNavBarSetIconCall = 0x44;
            auto navBarAttachPattern = dunia_pattern(
                "56 8B F1 83 7E 04 00 C6 46 51 01 74 3C 83 7E 08 00 74 36 83 7E 3C 08 72 05 8B 46 28 EB 03 8D 46 28 50 E8",
                "56 8B F1 83 7E 04 00 C6 46 39 01 74 3C 83 7E 08 00 74 36 83 7E 24 08 72 05 8B 46 10 EB 03 8D 46 10 50 E8");
            if (!navBarAttachPattern.empty())
            {
                static auto NavBarAttachHook = safetyhook::create_mid(navBarAttachPattern.get_first(nNavBarSetIconCall), [](SafetyHookContext& regs)
                {
                    auto pPrompt = reinterpret_cast<uint8_t*>(regs.esi);
                    if (!pPrompt)
                        return;

                    ApplyMenuGlyph(pPrompt);
                });

                // CNavBarModule::AttachPrompts entry, ECX the set. Detach is byte identical bar
                // the call inside it, so the match is settled on where that call goes.
                static constexpr ptrdiff_t nAttachAllCall = 0x15;
                auto pOnAttach = navBarAttachPattern.get_first();
                auto attachAllPattern = dunia_pattern("56 57 8B F1 33 FF 39 7E 08 76 1B 53 33 DB 8B FF 8B 4E 04 03 CB E8");

                for (size_t i = 0; i < attachAllPattern.size(); i++)
                {
                    auto pAttachAll = attachAllPattern.get(i).get<uint8_t>();
                    if (CallTarget(pAttachAll + nAttachAllCall) != reinterpret_cast<uintptr_t>(pOnAttach))
                        continue;

                    static auto NavBarAttachAllHook = safetyhook::create_mid(pAttachAll, [](SafetyHookContext& regs)
                    {
                        RememberPromptSet(reinterpret_cast<uint8_t*>(regs.ecx));
                    });

                    break;
                }

                // Attach is too early: a box's labels arrive after OnAttach. Placement is redone
                // every frame instead, at most four prompts. Hooked past the register saves.
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

                // Fired from the input drivers, so any thread; worst a race with the draw costs is
                // one frame of a half applied prompt.
                onInputDeviceChange() += []()
                {
                    RefreshMenuGlyphs();
                    RefreshBazaarGlyphs();
                };
            }

            // CHudPromptMgr::Update's per prompt loop; the call hands ECX over as the prompt.
            auto hudPromptPattern = dunia_pattern("8B FF D9 44 24 10 51 8B 0E D9 1C 24 03 CB E8 ? ? ? ? 83 C7 01 83 C3 60 3B 7E 04 72 E4");
            if (!hudPromptPattern.empty() && GetPadButtonImage)
            {
                static auto HudPromptHook = safetyhook::create_mid(hudPromptPattern.get_first(nHudPromptUpdateCall), [](SafetyHookContext& regs)
                {
                    auto pPrompt = reinterpret_cast<uint8_t*>(regs.ecx);
                    if (!pPrompt)
                        return;

                    // Nothing requested: the prompt is on its way out, its group hidden.
                    if (*reinterpret_cast<uint32_t*>(pPrompt + nHudPromptShown) == 0)
                        return;

                    auto pEntry = FindHudPrompt(*reinterpret_cast<uint32_t*>(pPrompt + nHudPromptName));
                    if (pEntry)
                        ApplyHudGlyph(pPrompt, *pEntry);
                });
            }

            // The ui/360.mgb document, caught on the glyph loader's own load.
            if (!glyphPattern.empty())
            {
                static auto UiDocumentHook = safetyhook::create_mid(glyphPattern.get_first(nUiDocumentLoaded), [](SafetyHookContext& regs)
                {
                    pUiDocument = reinterpret_cast<void*>(regs.eax);
                });
            }

            // CBazaarComputerUI::RefreshFocus, on both pages and every focus change, often enough
            // that the glyphs follow a device switch without a second hook.
            auto bazaarPattern = dunia_pattern("83 EC 08 53 55 56 57 8B F9 83 BF 80 01 00 00 00 0F 84 ? ? ? ? 83 BF 84 01 00 00 00");
            if (!bazaarPattern.empty())
            {
                auto pBazaar = reinterpret_cast<uint8_t*>(bazaarPattern.get_first());
                MakeElementName = reinterpret_cast<MakeElementName_t>(CallTarget(pBazaar + nMakeElementNameCall));
                FindElement = reinterpret_cast<FindElement_t>(CallTarget(pBazaar + nFindElementCall));

                static auto BazaarHook = safetyhook::create_mid(bazaarPattern.get_first(nBazaarRefreshThis),
                    [](SafetyHookContext&) { ApplyBazaarGlyphs(); });
            }

            // FUN_10720880, the loose lookup. Anchored whole: eight functions share its inner shape.
            auto findByNamePattern = dunia_pattern("8B 44 24 04 85 C0 56 8B F1 74 1A 50 8D 44 24 0C 50 E8 ? ? ? ? 83 C4 08 50 8B CE E8 ? ? ? ? 5E C2 04 00");
            if (!findByNamePattern.empty())
                FindElementByName = reinterpret_cast<FindElementByName_t>(findByNamePattern.get_first());

            // CBazaarComputerUI's enter and leave overrides, FUN_10734630 and FUN_10731EA0.
            auto bazaarEnterPattern = dunia_pattern("56 8B F1 E8 ? ? ? ? 80 BE 80 02 00 00 00 0F 84 ? ? ? ? A1 ? ? ? ? 53 57 6A 01 8B CE");
            if (!bazaarEnterPattern.empty())
            {
                // No refresh: the page's prompts attach after this and read the flag going in.
                static auto BazaarEnterHook = safetyhook::create_mid(bazaarEnterPattern.get_first(), [](SafetyHookContext&)
                {
                    bShopGlyphsOnPrompts = true;
                });
            }

            // CBazaarComputerUI::OnLeave.
            auto bazaarLeavePattern = dunia_pattern("83 EC 30 56 57 8B F9 83 7F 6C 02 0F 84 ? ? ? ? 6A 00 E8");
            if (!bazaarLeavePattern.empty())
            {
                // At the first instruction, while the tree is still whole.
                static auto BazaarLeaveHook = safetyhook::create_mid(bazaarLeavePattern.get_first(), [](SafetyHookContext&)
                {
                    bShopGlyphsOnPrompts = false;
                    RefreshMenuGlyphs();
                });
            }

            auto setCursorEnabledPattern = dunia_pattern("83 EC 14 8B 44 24 18 53 56 8B F1 50 8D 4C 24 0C 51 8B CE E8 ? ? ? ? 8B 10 8A 5C 24 20 33 C9 38 4C 24 24");
            if (!setCursorEnabledPattern.empty())
                SetCursorEnabled = reinterpret_cast<SetCursorEnabled_t>(setCursorEnabledPattern.get_first());

            // FUN_104F0FB0, the UI input dispatcher. Taken whole: the trigger case must answer
            // instead of the dispatcher, not alongside it.
            auto uiInputPattern = dunia_pattern("A1 ? ? ? ? 83 EC 24 53 56 33 DB 38 58 69 57 8B F9 74 1D 8B 48 78 3B CB 74 16");
            if (!uiInputPattern.empty() && SetCursorEnabled)
                UiInputHook = safetyhook::create_inline(uiInputPattern.get_first(), UiInput);

            // The dispatcher's own button down and button up calls, so both come off one anchor.
            static constexpr ptrdiff_t nUiMouseDownCall = 0x19;
            static constexpr ptrdiff_t nUiMouseUpCall = 0x43;
            auto uiMousePattern = dunia_pattern("8B 76 08 3B 35 ? ? ? ? 75 1E 8B 0D ? ? ? ? 8D 54 24 10 52 83 C1 04 E8");
            if (!uiMousePattern.empty())
            {
                auto pMatch = reinterpret_cast<uint8_t*>(uiMousePattern.get_first());
                UiMouseDown = reinterpret_cast<UiMouseEvent_t>(CallTarget(pMatch + nUiMouseDownCall));
                UiMouseUp = reinterpret_cast<UiMouseEvent_t>(CallTarget(pMatch + nUiMouseUpCall));
            }

            // The move, ahead of the pair above and off the same global.
            static constexpr ptrdiff_t nUiMouseMoveCall = 0x19;
            auto uiMovePattern = dunia_pattern("8B 46 04 3B 05 ? ? ? ? 75 1E 8D 4C 24 10 51 8B 0D ? ? ? ? 83 C1 04 E8");
            if (!uiMovePattern.empty())
            {
                auto pMatch = reinterpret_cast<uint8_t*>(uiMovePattern.get_first());
                UiMouseMove = reinterpret_cast<UiMouseEvent_t>(CallTarget(pMatch + nUiMouseMoveCall));
            }

            // A page's own mouse move, past its prologue: ECX still the page, event at +0x84
            // already in page space, so the difference from what was sent is the transform.
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

                    // The probe carries 0,0, so the page's copy is the transform negated.
                    nCalibrateX = -static_cast<int16_t>(pEvent[1] & 0xFFFF);
                    nCalibrateY = -static_cast<int16_t>(pEvent[1] >> 16);
                    bCalibrated = true;
                });
            }

            // CGameMessageBox::CGameMessageBox, ECX the box. The base vftable written here is
            // replaced by the actual class, so the kind is read later off the live object.
            if (auto* pMessageBoxCtor = dunia_find("83 EC 14 8B 44 24 18 53 55 56 57 50 8B F1 E8 ? ? ? ? C7 46 70 ? ? ? ? 8D 6E 74 8B CD C7 06"))
            {
                static auto MessageBoxCtorHook = safetyhook::create_mid(pMessageBoxCtor, [](SafetyHookContext& regs)
                {
                    auto pBox = reinterpret_cast<uint8_t*>(regs.ecx);
                    if (pBox && std::find(sLiveMessageBoxes.begin(), sLiveMessageBoxes.end(), pBox) == sLiveMessageBoxes.end())
                        sLiveMessageBoxes.push_back(pBox);
                });
            }

            // CGameMessageBox::~CGameMessageBox, first instruction, before the base vftable goes back.
            if (auto* pMessageBoxDtor = dunia_find("83 EC 18 56 8B F1 8D 8E 04 01 00 00 C7 06 ? ? ? ? C7 46 04 ? ? ? ? C7 46 70 ? ? ? ? E8"))
            {
                static auto MessageBoxDtorHook = safetyhook::create_mid(pMessageBoxDtor, [](SafetyHookContext& regs)
                {
                    auto pBox = reinterpret_cast<uint8_t*>(regs.ecx);
                    sLiveMessageBoxes.erase(std::remove(sLiveMessageBoxes.begin(), sLiveMessageBoxes.end(), pBox),
                        sLiveMessageBoxes.end());

                    // The draw puts the mask back, and the last box can be the last draw.
                    if (sLiveMessageBoxes.empty())
                        SetPadButtonMask(nNoPadButtonMask);
                });
            }
        };
    }
} ControllerPrompts;
