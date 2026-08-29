module;

#include <common.hxx>
#include <cstdint>

export module hudfixes;

import common;
import dunia;
import internalres;

// Layout

// magma::Package, through vtable +0x3C..+0x48.
static constexpr ptrdiff_t nPackageCanvasWidth = 0x54;
static constexpr ptrdiff_t nPackageCanvasHeight = 0x56;
static constexpr ptrdiff_t nPackageOriginX = 0x58;
static constexpr ptrdiff_t nPackageOriginY = 0x5A;

// Render context FUN_10AB59A0 fills before BeginScreen; read back as int16.
static constexpr ptrdiff_t nContextCanvasWidth = 0x34;
static constexpr ptrdiff_t nContextCanvasHeight = 0x36;
static constexpr ptrdiff_t nContextOriginX = 0x38;
static constexpr ptrdiff_t nContextOriginY = 0x3A;

// magma::Render quad vertices, page origin folded in.
static constexpr ptrdiff_t nRenderVertexX[4] = { 0x04, 0x1C, 0x34, 0x4C };
static constexpr ptrdiff_t nRenderVertexY[4] = { 0x08, 0x20, 0x38, 0x50 };
static constexpr ptrdiff_t nRenderOriginX = 0xE0;
static constexpr ptrdiff_t nRenderOriginY = 0xE4;

// Area transform translation at Render+0x80, column major. Shared by every quad of one element,
// so it doubles as element identity.
static constexpr ptrdiff_t nRenderMatrixTranslateX = 0xB0;
static constexpr ptrdiff_t nRenderMatrixTranslateY = 0xB4;

// magma display stack, EDI at the canvas hook. Page = [[EDI+0x4] + EBP*8], EBP the page index.
static constexpr ptrdiff_t nDisplayPageVector = 0x04;
static constexpr size_t nDisplayPageEntrySize = 8;

// Widget vtable +0x20, GetPackage. Plain getter, safe from a hook.
static constexpr size_t nWidgetVtableGetPackage = 0x20 / sizeof(void*);

// Display descriptor from FUN_1032D910. Static 1x1 stand-in until a device exists.
static constexpr ptrdiff_t nDisplayValid = 0x00;
static constexpr ptrdiff_t nDisplayWidth = 0x08;
static constexpr ptrdiff_t nDisplayHeight = 0x0C;
static constexpr ptrdiff_t nDisplayAspect = 0x10;

// HUD canvas height as a fraction of content height: console renders 1280x696 in a 720p signal.
static constexpr int nCanvasHeightNumerator = 29;
static constexpr int nCanvasHeightDenominator = 30;

// Centred: the reticle is authored on the content box centre, so -72 here dragged it too.
static constexpr int nContentOffsetY = 0;

// Row bands, authored y against console y:
//
//     banner     118 .. 167     console 199    +62
//     reticle    317 .. 407     console 359      0
//     health     664 .. 725     console 661    -72
//
// Bottom bound is per column; each column's cluster starts at a different height.
static constexpr int nTopZoneMax = 150;
static constexpr int nBottomZoneMinLeft = 400;
static constexpr int nBottomZoneMin = 620;
static constexpr int nBottomZoneMinRight = 600;

// Diamond block rides the left column, still 2 px right / 4 px high. Bounded above by the
// syringes at 620.
static constexpr int nDiamondZoneMax = 620;
static constexpr int nDiamondShiftX = -2;
static constexpr int nDiamondShiftY = 4;

// Right column: call icon sits 6.8 units in. 650 not 692, or the grenade at 663 moves too.
static constexpr int nCallIconZoneMax = 650;
static constexpr int nCallIconShiftX = 5;
static constexpr int nCallIconShiftY = -4;

static constexpr int nShiftTop = 62;
static constexpr int nShiftBottom = -72;

// Ammo block reads 3 px low once the health bar is on console's rows.
static constexpr int nShiftBottomRight = -75;

// Canvas width at which each side already sits where console puts it; the shift is half the
// difference from the real canvas. Two pairs: the HUD is authored twice, the same 960x720 of
// content in a 1280x800 widescreen canvas or a 1024x768 4:3 one.
//
//                     left extreme   right extreme   health bar anchors
//     widescreen         -136.8         1120.0          -140 .. 42
//     4:3                 -26.0          990.0           -26 .. 182
static constexpr int nDesignWidthLeft = 1437;
static constexpr int nDesignWidthRight = 1447;
static constexpr int nDesignWidthLeftSD = 1222;
static constexpr int nDesignWidthRightSD = 1193;

// Column boundaries. Left is 60 not 0 to keep all five health bar segments (-140..42) one side.
static constexpr int nLeftZoneMax = 60;
static constexpr int nRightZoneMin = 700;

// 4:3 bar runs to 182, so 60 leaves three segments behind; 300 alone catches the subtitle at 76,
// hence the row bound.
static constexpr int nLeftZoneMaxSD = 300;
static constexpr int nLeftZoneRowSD = 700;

// Canvas shorts are written and read back as int16.
static constexpr int nCanvasCeiling = 32767;

// Overlay packages: console, save notification, version line. Drawn over whatever page is up,
// reached by none of the offsets above.
static constexpr const char* pszOverlayPackages[] =
{
    "common.mgb",
    "common_mp.mgb",
};
static constexpr int nOverlayPackages = static_cast<int>(sizeof(pszOverlayPackages) / sizeof(pszOverlayPackages[0]));

// Filled by the lookup hook; a package carries no name.
static void* pOverlayPackages[nOverlayPackages] = {};

// Console page ends in pixels on a 720p capture, not a stretch factor (that would come off the
// authored canvas). One shift per end: the caret advances a glyph at a time and would walk out.
static constexpr float fOverlayLeftPixels = 70.0f;
static constexpr float fOverlayRightPixels = 189.0f;
static constexpr float fReferencePixelHeight = 720.0f;

static constexpr float fOverlayLeftReference = -90.0f;
static constexpr float fOverlayRightReference = 931.0f;

// 4:3 layout anchors these elsewhere.
static constexpr float fOverlayLeftReferenceSD = 20.0f;
static constexpr float fOverlayRightReferenceSD = 821.0f;

// Version line is the only strip element on the right edge; the bounds clear the text below it.
static constexpr float fOverlayRightZoneMin = 900.0f;
static constexpr float fOverlayRightZoneMinSD = 700.0f;

// Console strip is its own page, so all of it moves. Shape test: full canvas width, about half
// its height, drawn first. Full height is a dialog vignette, which must not move.
static constexpr float fConsoleStripMinHeight = 0.35f;
static constexpr float fConsoleStripMaxHeight = 0.65f;

// Other overlay pages move only the top left corner (save/load indicator); an extent test rather
// than an anchor band, or the sprites are left behind.
static constexpr float fCornerMaxX = 200.0f;
static constexpr float fCornerMaxY = 60.0f;

// Engine entry points

// 0x1032D910. No arguments, result in EAX.
using GetDisplayDescriptor_t = uint8_t* (__cdecl*)();

// 0x105355B0. __stdcall, MSVC8 std::string by pointer, returns the magma::Package or null.
using GetPackageForPath_t = void* (__stdcall*)(void*);

static GetDisplayDescriptor_t GetDisplayDescriptor = nullptr;
static SafetyHookInline GetPackageForPathHook{};

// Only the gameplay HUD is named: the edge offsets are measured against its authored layout. The
// canvas and backdrop remap are not package specific.
static constexpr const char* pszGameplayHudPackages[] =
{
    "hud.mgb",
    "hud_mp.mgb",
};
static constexpr int nGameplayHudPackages = static_cast<int>(sizeof(pszGameplayHudPackages) / sizeof(pszGameplayHudPackages[0]));

// Null until the HUD is asked for once; null only stops the offsets.
static void* pGameplayHudPackages[nGameplayHudPackages] = {};

// Pages the offsets apply to, learned from contents. The lookup hook empties this on a reload.
static constexpr int nMaxAnchorPages = 8;
static void* pAnchorPages[nMaxAnchorPages] = {};
static int nAnchorPages = 0;

// Identifying the packages

// MSVC8 std::string: size +0x14, capacity +0x18, buffer inline at +0x04 until capacity >= 16,
// then +0x04 is a pointer.
static const char* ReadDuniaString(uintptr_t pStr, char* pBuf, size_t nBufSize)
{
    pBuf[0] = '\0';
    if (!pStr || nBufSize < 2 || IsBadReadPtr((void*)pStr, 0x1C))
        return pBuf;

    auto nSize = *(uint32_t*)(pStr + 0x14);
    auto nCapacity = *(uint32_t*)(pStr + 0x18);
    if (nSize > 512 || nCapacity > 0x10000)
        return pBuf;

    auto pSrc = (nCapacity >= 16) ? *(const char**)(pStr + 4) : (const char*)(pStr + 4);
    if (!pSrc || IsBadReadPtr(pSrc, nSize))
        return pBuf;

    auto n = (nSize < nBufSize - 1) ? nSize : nBufSize - 1;
    for (size_t i = 0; i < n; i++)
        pBuf[i] = pSrc[i];
    pBuf[n] = '\0';
    return pBuf;
}

// FUN_10554FC0 expands the path to ui\localized\<branch>\<lang>\ui\, so compare the leaf only.
static const char* PathLeaf(const char* pszPath)
{
    const char* pLeaf = pszPath;
    for (const char* p = pszPath; *p != '\0'; p++)
    {
        if (*p == '/' || *p == '\\')
            pLeaf = p + 1;
    }

    return pLeaf;
}

static int PackageSlot(const char* pszPath, const char* const* pszNames, int nNames)
{
    const char* pLeaf = PathLeaf(pszPath);

    for (int i = 0; i < nNames; i++)
    {
        if (_stricmp(pLeaf, pszNames[i]) == 0)
            return i;
    }

    return -1;
}

static bool IsKnownPackage(const void* pPackage, void* const* pKnown, int nKnown)
{
    if (pPackage == nullptr)
        return false;

    for (int i = 0; i < nKnown; i++)
    {
        if (pKnown[i] == pPackage)
            return true;
    }

    return false;
}

static void* __stdcall GetPackageForPath(void* pPath)
{
    auto* pPackage = GetPackageForPathHook.stdcall<void*>(pPath);

    char szPath[MAX_PATH];
    ReadDuniaString(reinterpret_cast<uintptr_t>(pPath), szPath, sizeof(szPath));

    if (pPackage != nullptr && szPath[0] != '\0')
    {
        const int nHudSlot = PackageSlot(szPath, pszGameplayHudPackages, nGameplayHudPackages);
        if (nHudSlot >= 0 && pGameplayHudPackages[nHudSlot] != pPackage)
        {
            // A reload hands back different pages.
            pGameplayHudPackages[nHudSlot] = pPackage;
            nAnchorPages = 0;
        }

        const int nOverlaySlot = PackageSlot(szPath, pszOverlayPackages, nOverlayPackages);
        if (nOverlaySlot >= 0)
            pOverlayPackages[nOverlaySlot] = pPackage;
    }

    return pPackage;
}

// The canvas

enum class PageKind { GameplayHud, Menu };

static PageKind PageKindOf(const void* pPackage)
{
    return IsKnownPackage(pPackage, pGameplayHudPackages, nGameplayHudPackages) ? PageKind::GameplayHud : PageKind::Menu;
}

// The composed frame, not the window: it is fitted to the output whole. FUN_1032D910 gives the
// window and is the fallback. Exported: the controller prompts size glyphs in the same unit space.
export bool GetFrameAspect(float& fAspect)
{
    uint32_t nFrameWidth = 0;
    uint32_t nFrameHeight = 0;
    if (GetInternalFrameSize(nFrameWidth, nFrameHeight) && nFrameWidth > 1 && nFrameHeight > 1)
    {
        fAspect = static_cast<float>(nFrameWidth) / static_cast<float>(nFrameHeight);
        return std::isfinite(fAspect) && fAspect > 0.0f;
    }

    if (GetDisplayDescriptor == nullptr)
        return false;

    auto* pDisplay = GetDisplayDescriptor();
    if (pDisplay == nullptr || pDisplay[nDisplayValid] == 0)
        return false;

    const auto nDisplayWidthPx = *reinterpret_cast<int32_t*>(pDisplay + nDisplayWidth);
    const auto nDisplayHeightPx = *reinterpret_cast<int32_t*>(pDisplay + nDisplayHeight);

    // The 1x1 stand-in reports aspect 1.0.
    if (nDisplayWidthPx < 2 || nDisplayHeightPx < 2)
        return false;

    fAspect = *reinterpret_cast<float*>(pDisplay + nDisplayAspect);
    if (!std::isfinite(fAspect) || fAspect <= 0.0f)
        fAspect = static_cast<float>(nDisplayWidthPx) / static_cast<float>(nDisplayHeightPx);

    return std::isfinite(fAspect) && fAspect > 0.0f;
}

struct Canvas
{
    int nWidth = 0;
    int nHeight = 0;
    int nOriginX = 0;
    int nOriginY = 0;
};

// Authored layout: 4:3 is 1024x768, widescreen 1280x800. Only the edge offsets care.
static bool IsWidescreenAuthored(const Canvas& authored)
{
    return authored.nWidth * 3 > authored.nHeight * 4;
}


// Off the Package, not the context: the hook writes the context, so a page reaching BeginScreen
// twice would compound the correction.
static Canvas ReadAuthoredCanvas(const uint8_t* pPackage)
{
    Canvas authored;
    authored.nWidth = *reinterpret_cast<const uint16_t*>(pPackage + nPackageCanvasWidth);
    authored.nHeight = *reinterpret_cast<const uint16_t*>(pPackage + nPackageCanvasHeight);
    authored.nOriginX = *reinterpret_cast<const uint16_t*>(pPackage + nPackageOriginX);
    authored.nOriginY = *reinterpret_cast<const uint16_t*>(pPackage + nPackageOriginY);
    return authored;
}

// Height is the only HUD/menu difference; width follows from aspect, keeping px/unit square.
static bool BuildPageCanvas(const Canvas& authored, float fAspect, PageKind kind, Canvas& fixed)
{
    const int nContentWidth = authored.nWidth - 2 * authored.nOriginX;
    const int nContentHeight = authored.nHeight - 2 * authored.nOriginY;

    if (nContentWidth <= 0 || nContentHeight <= 0)
        return false;

    if (!std::isfinite(fAspect) || fAspect <= 0.0f)
        return false;

    // HUD height comes off the content box, not the authored canvas: both authored canvases hold
    // the same 960x720, so it stays 696 at every aspect.
    int nHeight = (kind == PageKind::GameplayHud)
        ? (nContentHeight * nCanvasHeightNumerator + nCanvasHeightDenominator / 2) / nCanvasHeightDenominator
        : authored.nHeight;

    int nWidth = static_cast<int>(nHeight * fAspect + 0.5f);

    // Frame narrower than the content box: fit the box instead and scale the page down uniformly.
    if (nWidth < nContentWidth)
    {
        nWidth = nContentWidth;
        nHeight = static_cast<int>(nWidth / fAspect + 0.5f);
    }

    if (nWidth < 1 || nHeight < 1 || nWidth > nCanvasCeiling || nHeight > nCanvasCeiling)
        return false;

    fixed.nWidth = nWidth;
    fixed.nHeight = nHeight;
    fixed.nOriginX = (nWidth - nContentWidth) / 2;

    // Not clamped to zero: the HUD is authored past the box, so a negative origin is correct.
    fixed.nOriginY = (nHeight - nContentHeight) / 2 + nContentOffsetY;
    return true;
}

// Drawing

static bool bPageCorrected = false;
static bool bGameplayHudPage = false;
static bool bAnchorPage = false;
static bool bOverlayPage = false;

static bool bConsoleStripPage = false;

static float fOverlayDeltaLeft = 0.0f;
static float fOverlayDeltaRight = 0.0f;
static float fOverlayRightZone = fOverlayRightZoneMin;

// Authored sizes double as the backdrop test, so they stay extents.
static float fAuthoredCanvasWidth = 0.0f;
static float fAuthoredCanvasHeight = 0.0f;
static float fBackdropOffsetX = 0.0f;
static float fBackdropOffsetY = 0.0f;
static float fBackdropScaleX = 1.0f;
static float fBackdropScaleY = 1.0f;

static float fContentCentre = 0.0f;
static float fDeltaLeft = 0.0f;
static float fDeltaRight = 0.0f;
static bool bWidescreenLayout = true;

// Widget::GetPackage through the vtable; no static call site to match.
static void* GetPagePackage(void* pPage)
{
    if (pPage == nullptr)
        return nullptr;

    auto** pVtable = *reinterpret_cast<void***>(pPage);
    if (pVtable == nullptr)
        return nullptr;

    using GetPackage_t = void* (__fastcall*)(void*, void*);
    return reinterpret_cast<GetPackage_t>(pVtable[nWidgetVtableGetPackage])(pPage, nullptr);
}

static void* PageAt(uint8_t* pDisplay, uint32_t nIndex)
{
    auto* pEntries = *reinterpret_cast<uint8_t**>(pDisplay + nDisplayPageVector);
    if (pEntries == nullptr)
        return nullptr;

    return *reinterpret_cast<void**>(pEntries + nIndex * nDisplayPageEntrySize);
}

static void* pCurrentPage = nullptr;

// The reticle's origin walks with weapon sway and crossed the top boundary. 150 clears the marks
// at 459..484 and stops short of the ammo at 981.
static constexpr float fCentreDetailMargin = 150.0f;
static constexpr float fCentreDetailExtent = 24.0f;

static bool IsCentredDetail(float fGroupX, float fWidth, float fHeight)
{
    if (fContentCentre <= 0.0f)
        return false;

    const float fOffset = fGroupX - fContentCentre;
    return fOffset > -fCentreDetailMargin && fOffset < fCentreDetailMargin
        && fWidth <= fCentreDetailExtent && fHeight <= fCentreDetailExtent;
}

static constexpr float fOutsideBoxMargin = 20.0f;

static bool IsAnchorPage(void* pPage)
{
    if (pPage == nullptr)
        return false;

    for (int i = 0; i < nAnchorPages; i++)
    {
        if (pAnchorPages[i] == pPage)
            return true;
    }

    return false;
}

static void RememberAnchorPage(void* pPage)
{
    if (pPage == nullptr || IsAnchorPage(pPage) || nAnchorPages >= nMaxAnchorPages)
        return;

    pAnchorPages[nAnchorPages++] = pPage;
}

// Anchor page: an element reaching past the content box and narrower than it. Both halves needed
// -- stack order cannot separate the pages, and reaching past alone catches every backdrop.
static bool IsAnchoredElement(float fOriginX, float fWidth)
{
    const float fBox = fContentCentre * 2.0f;
    if (fBox <= 0.0f)
        return false;

    if (fOriginX >= -fOutsideBoxMargin && fOriginX <= fBox + fOutsideBoxMargin)
        return false;

    return fWidth < fBox;
}

// 0x10AB5AEC in FUN_10AB59A0's per-page block. ESI Package, EBX context, EDI display stack,
// EBP page index.
static void ApplyCanvas(SafetyHookContext& regs)
{
    auto* pPackage = reinterpret_cast<uint8_t*>(regs.esi);

    bPageCorrected = pPackage != nullptr;
    bGameplayHudPage = IsKnownPackage(pPackage, pGameplayHudPackages, nGameplayHudPackages);
    bAnchorPage = false;
    bOverlayPage = bPageCorrected && !bGameplayHudPage && IsKnownPackage(pPackage, pOverlayPackages, nOverlayPackages);
    bConsoleStripPage = false;

    if (!bPageCorrected)
        return;

    float fAspect = 0.0f;
    if (!GetFrameAspect(fAspect))
    {
        bPageCorrected = false;
        bGameplayHudPage = false;
        bOverlayPage = false;
        return;
    }

    const Canvas authored = ReadAuthoredCanvas(pPackage);

    Canvas fixed;
    if (!BuildPageCanvas(authored, fAspect, PageKindOf(pPackage), fixed))
    {
        bPageCorrected = false;
        bGameplayHudPage = false;
        bOverlayPage = false;
        return;
    }

    auto* pContext = reinterpret_cast<uint8_t*>(regs.ebx);
    *reinterpret_cast<int16_t*>(pContext + nContextCanvasWidth) = static_cast<int16_t>(fixed.nWidth);
    *reinterpret_cast<int16_t*>(pContext + nContextCanvasHeight) = static_cast<int16_t>(fixed.nHeight);
    *reinterpret_cast<int16_t*>(pContext + nContextOriginX) = static_cast<int16_t>(fixed.nOriginX);
    *reinterpret_cast<int16_t*>(pContext + nContextOriginY) = static_cast<int16_t>(fixed.nOriginY);

    fContentCentre = (authored.nWidth - 2 * authored.nOriginX) * 0.5f;
    bWidescreenLayout = IsWidescreenAuthored(authored);
    fDeltaLeft = -(fixed.nWidth - (bWidescreenLayout ? nDesignWidthLeft : nDesignWidthLeftSD)) * 0.5f;
    fDeltaRight = (fixed.nWidth - (bWidescreenLayout ? nDesignWidthRight : nDesignWidthRightSD)) * 0.5f;

    // Those pixel counts in corrected-canvas units; the canvas is fitted to frame height.
    const float fUnitsPerPixel = static_cast<float>(fixed.nHeight) / fReferencePixelHeight;
    const float fFixedOriginX = static_cast<float>(fixed.nOriginX);
    const bool bWidescreenOverlay = IsWidescreenAuthored(authored);
    const float fLeftReference = bWidescreenOverlay ? fOverlayLeftReference : fOverlayLeftReferenceSD;
    const float fRightReference = bWidescreenOverlay ? fOverlayRightReference : fOverlayRightReferenceSD;
    fOverlayRightZone = bWidescreenOverlay ? fOverlayRightZoneMin : fOverlayRightZoneMinSD;

    fOverlayDeltaLeft = fOverlayLeftPixels * fUnitsPerPixel - fFixedOriginX - fLeftReference;
    fOverlayDeltaRight = static_cast<float>(fixed.nWidth) - fOverlayRightPixels * fUnitsPerPixel
        - fFixedOriginX - fRightReference;

    fAuthoredCanvasWidth = static_cast<float>(authored.nWidth);
    fAuthoredCanvasHeight = static_cast<float>(authored.nHeight);
    fBackdropOffsetX = static_cast<float>(authored.nOriginX - fixed.nOriginX);
    fBackdropOffsetY = static_cast<float>(authored.nOriginY - fixed.nOriginY);
    fBackdropScaleX = (authored.nWidth > 0) ? (static_cast<float>(fixed.nWidth) / fAuthoredCanvasWidth) : 1.0f;
    fBackdropScaleY = (authored.nHeight > 0) ? (static_cast<float>(fixed.nHeight) / fAuthoredCanvasHeight) : 1.0f;

    pCurrentPage = PageAt(reinterpret_cast<uint8_t*>(regs.edi), static_cast<uint32_t>(regs.ebp));
    bAnchorPage = IsAnchorPage(pCurrentPage);

}

static int BandOf(float fPoint, float fLowBoundary, float fHighBoundary)
{
    if (fPoint < fLowBoundary)
        return -1;
    if (fPoint > fHighBoundary)
        return 1;
    return 0;
}

static void Move(uint8_t* pRender, const ptrdiff_t (&nOffsets)[4], float fDelta)
{
    if (fDelta == 0.0f)
        return;

    for (int i = 0; i < 4; i++)
        *reinterpret_cast<float*>(pRender + nOffsets[i]) += fDelta;
}

static void Remap(uint8_t* pRender, const ptrdiff_t (&nOffsets)[4], float fOffset, float fScale)
{
    for (int i = 0; i < 4; i++)
    {
        auto& f = *reinterpret_cast<float*>(pRender + nOffsets[i]);
        f = (f + fOffset) * fScale;
    }
}

// A quad covering the authored canvas is the canvas, so remap it rather than scale it:
//
//     x' = (x + authoredOriginX - originX) * canvasWidth  / authoredCanvasWidth
//     y' = (y + authoredOriginY - originY) * canvasHeight / authoredCanvasHeight
//
// Per axis: the console strip spans the width and half the height. Covering, not merely large --
// the tutorial vignette over-scans on purpose.
static bool IsBackdropAxis(float fExtent, float fAuthoredExtent)
{
    return fAuthoredExtent > 0.0f && fExtent >= fAuthoredExtent;
}

// 0x105FAD67, between the last vertex write and the divide. ESI is magma::Render.
// Quad not widget: magma::Area has no extent, and every quad of an element shares its translation.
static void ShiftQuad(SafetyHookContext& regs)
{
    if (!bPageCorrected)
        return;

    auto* pRender = reinterpret_cast<uint8_t*>(regs.esi);

    const float fGroupX = *reinterpret_cast<float*>(pRender + nRenderMatrixTranslateX);
    const float fGroupY = *reinterpret_cast<float*>(pRender + nRenderMatrixTranslateY);

    // Page origin taken back out, so the extent matches the origin units.
    float fLeft, fRight, fTop, fBottom;
    {
        const float fOx = *reinterpret_cast<float*>(pRender + nRenderOriginX);
        const float fOy = *reinterpret_cast<float*>(pRender + nRenderOriginY);

        fLeft = fRight = *reinterpret_cast<float*>(pRender + nRenderVertexX[0]) - fOx;
        fTop = fBottom = *reinterpret_cast<float*>(pRender + nRenderVertexY[0]) - fOy;
        for (int i = 1; i < 4; i++)
        {
            const float x = *reinterpret_cast<float*>(pRender + nRenderVertexX[i]) - fOx;
            const float y = *reinterpret_cast<float*>(pRender + nRenderVertexY[i]) - fOy;
            if (x < fLeft) fLeft = x;
            if (x > fRight) fRight = x;
            if (y < fTop) fTop = y;
            if (y > fBottom) fBottom = y;
        }
    }

    // Latched: only quads ahead of the first anchored element on the page's first frame miss.
    if (!bAnchorPage && bGameplayHudPage && IsAnchoredElement(fGroupX, fRight - fLeft))
    {
        RememberAnchorPage(pCurrentPage);
        bAnchorPage = true;
    }

    const bool bBackdropX = IsBackdropAxis(fRight - fLeft, fAuthoredCanvasWidth);
    const bool bBackdropY = IsBackdropAxis(fBottom - fTop, fAuthoredCanvasHeight);
    if (bBackdropX || bBackdropY)
    {
        // Drawn first on its page, so the flag is up when the console's quads arrive.
        const float fHeight = fBottom - fTop;
        if (bOverlayPage && bBackdropX
            && fHeight >= fAuthoredCanvasHeight * fConsoleStripMinHeight
            && fHeight <= fAuthoredCanvasHeight * fConsoleStripMaxHeight)
        {
            bConsoleStripPage = true;
        }

        if (bBackdropX)
            Remap(pRender, nRenderVertexX, fBackdropOffsetX, fBackdropScaleX);
        if (bBackdropY)
            Remap(pRender, nRenderVertexY, fBackdropOffsetY, fBackdropScaleY);
        return;
    }

    if (bOverlayPage)
    {
        // Strip page: version line right, everything else left. Other overlay pages: corner only.
        if (bConsoleStripPage)
            Move(pRender, nRenderVertexX, (fGroupX > fOverlayRightZone) ? fOverlayDeltaRight : fOverlayDeltaLeft);
        else if (fRight <= fCornerMaxX && fBottom <= fCornerMaxY)
            Move(pRender, nRenderVertexX, fOverlayDeltaLeft);

        return;
    }

    // Menus and dialogs stop here.
    if (!bGameplayHudPage || !bAnchorPage)
        return;

    if (IsCentredDetail(fGroupX, fRight - fLeft, fBottom - fTop))
        return;

    int nBandX = BandOf(fGroupX, static_cast<float>(nLeftZoneMax), static_cast<float>(nRightZoneMin));

    // 4:3 health bar: anchored past the boundary, and only in its own row.
    if (nBandX == 0 && !bWidescreenLayout
        && fGroupX < nLeftZoneMaxSD && fGroupY >= nLeftZoneRowSD)
    {
        nBandX = -1;
    }

    const int nBottomBoundary = (nBandX < 0) ? nBottomZoneMinLeft
        : (nBandX > 0) ? nBottomZoneMinRight
        : nBottomZoneMin;
    const int nBandY = BandOf(fGroupY, static_cast<float>(nTopZoneMax), static_cast<float>(nBottomBoundary));

    float fDeltaX = (nBandX < 0) ? fDeltaLeft : (nBandX > 0) ? fDeltaRight : 0.0f;

    float fDeltaY = 0.0f;
    if (nBandY < 0)
        fDeltaY = static_cast<float>(nShiftTop);
    else if (nBandY > 0)
        fDeltaY = static_cast<float>((nBandX > 0) ? nShiftBottomRight : nShiftBottom);

    if (nBandX < 0 && nBandY > 0 && fGroupY < nDiamondZoneMax)
    {
        fDeltaX += nDiamondShiftX;
        fDeltaY += nDiamondShiftY;
    }
    else if (nBandX > 0 && nBandY > 0 && fGroupY < nCallIconZoneMax)
    {
        fDeltaX += nCallIconShiftX;
        fDeltaY += nCallIconShiftY;
    }

    Move(pRender, nRenderVertexX, fDeltaX);
    Move(pRender, nRenderVertexY, fDeltaY);
}

// The cursor

// Cursor, drawn after the page loop; ESI topmost Package, EBP display. Last write to the canvas
// fields each frame, and page flags are cleared here or the cursor quad hits the HUD band tests.
static void ApplyCursorCanvas(SafetyHookContext& regs)
{
    // Not a page of the loop, so nothing here may be shifted.
    bPageCorrected = false;
    bGameplayHudPage = false;
    bAnchorPage = false;
    bOverlayPage = false;
    bConsoleStripPage = false;

    auto* pPackage = reinterpret_cast<uint8_t*>(regs.esi);
    if (pPackage == nullptr)
        return;

    float fAspect = 0.0f;
    if (!GetFrameAspect(fAspect))
        return;

    const Canvas authored = ReadAuthoredCanvas(pPackage);

    Canvas fixed;
    if (!BuildPageCanvas(authored, fAspect, PageKindOf(pPackage), fixed))
        return;

    // Width and height only: the origins here are zero by design, the cursor being in raw units.
    auto* pContext = reinterpret_cast<uint8_t*>(regs.ebp);
    *reinterpret_cast<int16_t*>(pContext + nContextCanvasWidth) = static_cast<int16_t>(fixed.nWidth);
    *reinterpret_cast<int16_t*>(pContext + nContextCanvasHeight) = static_cast<int16_t>(fixed.nHeight);
}

// The click boxes

// 0x10AA16C0. __thiscall with RET 4, so __fastcall with an unused EDX is the same frame. Converts
// events with the authored origin (Package vtable +0x44/+0x48), putting hit boxes 22 left, 52 up.
static SafetyHookInline PageEventToContentHook{};

// One dword at +0x04, x in the low short and y in the high one.
static constexpr ptrdiff_t nEventPosition = 0x04;

static void __fastcall PageEventToContent(void* pPage, void* pEdx, uint8_t* pEvent)
{
    PageEventToContentHook.fastcall(pPage, pEdx, pEvent);

    if (pEvent == nullptr)
        return;

    auto* pPackage = reinterpret_cast<uint8_t*>(GetPagePackage(pPage));
    if (pPackage == nullptr)
        return;

    // This page's own package: input order is not dependable, canvases need not match.
    float fAspect = 0.0f;
    if (!GetFrameAspect(fAspect))
        return;

    const Canvas authored = ReadAuthoredCanvas(pPackage);

    Canvas fixed;
    if (!BuildPageCanvas(authored, fAspect, PageKindOf(pPackage), fixed))
        return;

    auto* pPosition = reinterpret_cast<int16_t*>(pEvent + nEventPosition);
    pPosition[0] += static_cast<int16_t>(authored.nOriginX - fixed.nOriginX);
    pPosition[1] += static_cast<int16_t>(authored.nOriginY - fixed.nOriginY);
}

class HudFixes
{
public:
    HudFixes()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            // 0x1032D910.
            auto displayPattern = dunia_pattern("33 C0 39 05 ? ? ? ? 74 09 A1 ? ? ? ? 83 C0 14 C3");

            // 0x105355B0, prologue through the empty-string proxy and 0xF capacity.
            auto lookupPattern = dunia_pattern(
                "81 EC 9C 00 00 00 53 56 57 8D 44 24 0F 50 8D 4C 24 34 33 DB BF ? ? ? ? BE 0F 00 00 00 51");

            // FUN_10AB59A0's per-page block. Hook site 0x10AB5AEC, thirteen bytes in.
            auto beginScreenPattern = dunia_pattern("0F B7 4C 24 10 66 89 43 38 66 89 4B 3A 8B 0D");

            // FUN_105FAB60's tail. Hook site 0x105FAD67, eighteen bytes in, after the last vertex
            // write. safetyhook's Context32 carries xmm0..xmm7, so the float run survives.
            auto quadPattern = dunia_pattern(
                "F3 0F 10 0D ? ? ? ? F3 0F 11 46 4C F3 0F 11 56 54 F3 0F 10 96 F0 00 00 00");

            // FUN_10AB59A0's cursor block. Hook site 0x10AB5B7E, seventeen bytes in; word stores
            // to EBP tell it from the loop's EBX.
            auto cursorPattern = dunia_pattern(
                "8B 06 8B 50 3C 8B CE FF D2 66 89 45 34 66 89 5D 36 8B 0D");

            // 0x10AA16C0, whole function.
            auto eventPattern = dunia_pattern(
                "8B 01 8B 50 20 56 57 FF D2 8B 7C 24 0C 8B F0 8B 47 04 8B 16 89 44 24 0C 8B 42 44 8B CE FF D0");

            if (displayPattern.empty() || lookupPattern.empty() || beginScreenPattern.empty() || quadPattern.empty())
                return;

            GetDisplayDescriptor = reinterpret_cast<GetDisplayDescriptor_t>(displayPattern.get_first());

            GetPackageForPathHook = safetyhook::create_inline(lookupPattern.get_first(), GetPackageForPath);

            static auto BeginScreenHook = safetyhook::create_mid(beginScreenPattern.get_first(0x0D), ApplyCanvas);
            static auto QuadHook = safetyhook::create_mid(quadPattern.get_first(0x12), ShiftQuad);

            // Independent of the above, so a miss costs only its own fix.
            if (!cursorPattern.empty())
            {
                static auto CursorCanvasHook = safetyhook::create_mid(cursorPattern.get_first(0x11), ApplyCursorCanvas);
            }

            if (!eventPattern.empty())
                PageEventToContentHook = safetyhook::create_inline(eventPattern.get_first(0), PageEventToContent);
        };
    }
} HudFixes;
