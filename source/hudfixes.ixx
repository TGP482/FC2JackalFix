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

// Render context FUN_10AB59A0 fills before BeginScreen; FUN_105FA9C0 reads it back as int16 into
// Render+0xD8/0xD4/0xE0/0xE4.
static constexpr ptrdiff_t nContextCanvasWidth = 0x34;
static constexpr ptrdiff_t nContextCanvasHeight = 0x36;
static constexpr ptrdiff_t nContextOriginX = 0x38;
static constexpr ptrdiff_t nContextOriginY = 0x3A;

// magma::Render quad vertices just before the divide, page origin already folded in.
static constexpr ptrdiff_t nRenderVertexX[4] = { 0x04, 0x1C, 0x34, 0x4C };
static constexpr ptrdiff_t nRenderVertexY[4] = { 0x08, 0x20, 0x38, 0x50 };
static constexpr ptrdiff_t nRenderOriginX = 0xE0;
static constexpr ptrdiff_t nRenderOriginY = 0xE4;

// Translation of the Area transform at Render+0x80; FUN_10065130 is column major, so m[12..14] is
// +0xB0/+0xB4. Shared by every quad of one element, so it works as an element identity.
static constexpr ptrdiff_t nRenderMatrixTranslateX = 0xB0;
static constexpr ptrdiff_t nRenderMatrixTranslateY = 0xB4;

// magma's display stack, EDI at the canvas hook. Page = [[EDI+0x4] + EBP*8], EBP the page index.
static constexpr ptrdiff_t nDisplayPageVector = 0x04;
static constexpr size_t nDisplayPageEntrySize = 8;

// Widget vtable +0x20, GetPackage. A plain getter, safe to call from a hook.
static constexpr size_t nWidgetVtableGetPackage = 0x20 / sizeof(void*);

// Display descriptor from FUN_1032D910. A static 1x1 stand-in until a device exists.
static constexpr ptrdiff_t nDisplayValid = 0x00;
static constexpr ptrdiff_t nDisplayWidth = 0x08;
static constexpr ptrdiff_t nDisplayHeight = 0x0C;
static constexpr ptrdiff_t nDisplayAspect = 0x10;

// HUD canvas height as a fraction of authored content height. Console renders 1280x696 in a 720p
// signal, so its HUD is sized against 696 of the 720 unit box; the full 720 drew it 720/696 small.
static constexpr int nCanvasHeightNumerator = 29;
static constexpr int nCanvasHeightDenominator = 30;

// Origin stays centred: the reticle is authored on the content box centre, and an earlier -72 here
// placed the health bar but dragged the reticle too.
static constexpr int nContentOffsetY = 0;

// Row bands. Console runs banner to health bar in 466 px against this package's 571, so top and
// bottom clusters move, middle stays:
//
//     banner     authored y 118 .. 167     console 199    +62
//     reticle    authored y 317 .. 407     console 359      0
//     health     authored y 664 .. 725     console 661    -72
//
// Boundaries must fall in gaps between elements. Origins: banner backing -274.4, banner text 105,
// crest 176..250, reticle and weapon swap 444..541.5, call icon 612..682, ammo and health 702..733.
// Bottom bound is per column since each column's cluster starts at a different height.
static constexpr int nTopZoneMax = 150;
static constexpr int nBottomZoneMinLeft = 400;
static constexpr int nBottomZoneMin = 620;
static constexpr int nBottomZoneMinRight = 600;

// The diamond block rides the left column and still lands 2 px right, 4 px high of console.
// Bounded above by the syringes at 620, which the column shift alone gets right.
static constexpr int nDiamondZoneMax = 620;
static constexpr int nDiamondShiftX = -2;
static constexpr int nDiamondShiftY = 4;

// Right column: the call icon sits 6.8 units in from console. 650 not 692, or the grenade at 663
// moves too.
static constexpr int nCallIconZoneMax = 650;
static constexpr int nCallIconShiftX = 5;
static constexpr int nCallIconShiftY = -4;

static constexpr int nShiftTop = 62;
static constexpr int nShiftBottom = -72;

// With the health bar on console's rows the ammo block still reads 3 px low.
static constexpr int nShiftBottomRight = -75;

// Canvas width at which each side already sits where console puts it. The shift is half the
// difference from the real canvas, so originX plus shift is constant at every width, holding the
// corners at 105 px and 86 px at every aspect.
//
// Two pairs, because the engine authors the HUD twice and picks by display (the SafeAreaBlack-
// BorderPercent HDX/HDY/SDX/SDY settings): the same 960x720 of content inside either a 1280x800
// widescreen canvas or a 1024x768 4:3 one, each solved against its own extremes:
//
//                     left extreme   right extreme   health bar anchors
//     widescreen         -136.8         1120.0          -140 .. 42
//     4:3                 -26.0          990.0           -26 .. 182
static constexpr int nDesignWidthLeft = 1437;
static constexpr int nDesignWidthRight = 1447;
static constexpr int nDesignWidthLeftSD = 1222;
static constexpr int nDesignWidthRightSD = 1193;

// Column boundaries. Widescreen origins: banner 82, 100 and 482, reticle 459..484, ammo 981 up.
// Left is 60 not 0 to keep all five health bar segments (-140..42) on one side.
static constexpr int nLeftZoneMax = 60;
static constexpr int nRightZoneMin = 700;

// The 4:3 bar runs to 182, so 60 leaves three segments behind; 300 alone catches the subtitle
// anchored at 76. Hence the row bound: segments at y 714, strip 703, subtitle 679, nothing else.
static constexpr int nLeftZoneMaxSD = 300;
static constexpr int nLeftZoneRowSD = 700;

// Canvas shorts are written and read back as int16.
static constexpr int nCanvasCeiling = 32767;

// Overlay packages. Console, save notification and version line are authored on common.mgb, drawn
// over whatever page is up, and reached by none of the offsets above.
static constexpr const char* pszOverlayPackages[] =
{
    "common.mgb",
    "common_mp.mgb",
};
static constexpr int nOverlayPackages = static_cast<int>(sizeof(pszOverlayPackages) / sizeof(pszOverlayPackages[0]));

// Learned from the same lookup as the HUD's: a package carries no name.
static void* pOverlayPackages[nOverlayPackages] = {};

// Where the console page's two ends belong, in pixels on a 720p capture. Pixels not a stretch
// factor, since the stretch would come off the authored canvas, which changes with the display. One
// shift per end, not one per element anchor: the caret anchors itself and advances a glyph at a
// time, so it would walk out from under the text.
static constexpr float fOverlayLeftPixels = 70.0f;
static constexpr float fOverlayRightPixels = 189.0f;
static constexpr float fReferencePixelHeight = 720.0f;

static constexpr float fOverlayLeftReference = -90.0f;
static constexpr float fOverlayRightReference = 931.0f;

// Same for the 4:3 layout, which anchors these elements elsewhere.
static constexpr float fOverlayLeftReferenceSD = 20.0f;
static constexpr float fOverlayRightReferenceSD = 821.0f;

// The version line is the only strip element belonging to the right edge. 900 clears widescreen
// text ending at 327; 4:3 anchors the line at 821, its text stops at 319.
static constexpr float fOverlayRightZoneMin = 900.0f;
static constexpr float fOverlayRightZoneMinSD = 700.0f;

// The console strip is a page of its own, so all of it moves. Identified by shape: full canvas
// width, about half its height, drawn first. Full height is a dialog's vignette instead, and moving
// that page takes the dialog off centre.
static constexpr float fConsoleStripMinHeight = 0.35f;
static constexpr float fConsoleStripMaxHeight = 0.65f;

// Every other overlay page moves only its top left corner, where the save/load indicator is: pill
// at -92.5..23.5, spinner at -90.6..-58.2 and a caption, both sprites anchored right of the caption,
// so an anchor band or extent test takes the text and leaves the artwork.
static constexpr float fCornerMaxX = 200.0f;
static constexpr float fCornerMaxY = 60.0f;

// Engine entry points

// 0x1032D910. No arguments, result in EAX.
using GetDisplayDescriptor_t = uint8_t* (__cdecl*)();

// 0x105355B0. __stdcall, MSVC8 std::string by pointer, returns the magma::Package or null.
using GetPackageForPath_t = void* (__stdcall*)(void*);

static GetDisplayDescriptor_t GetDisplayDescriptor = nullptr;
static SafetyHookInline GetPackageForPathHook{};

// Only the gameplay HUD is named, because the edge offsets are a table measured against its
// authored layout. The canvas and backdrop remap are not package specific; naming pages for those
// left a list always missing an entry (hud.mgb alone left the tutorial pop-up stretched).
static constexpr const char* pszGameplayHudPackages[] =
{
    "hud.mgb",
    "hud_mp.mgb",
};
static constexpr int nGameplayHudPackages = static_cast<int>(sizeof(pszGameplayHudPackages) / sizeof(pszGameplayHudPackages[0]));

// Filled by the lookup hook, null until the HUD is asked for once. Null only stops the offsets.
static void* pGameplayHudPackages[nGameplayHudPackages] = {};

// Pages of that package the offsets apply to, learned from their contents; one in practice. Kept
// here because the lookup hook empties it on a reload.
static constexpr int nMaxAnchorPages = 8;
static void* pAnchorPages[nMaxAnchorPages] = {};
static int nAnchorPages = 0;

// Identifying the packages

// MSVC8 std::string: size at +0x14, capacity at +0x18, buffer inline at +0x04 until it outgrows 16
// bytes and +0x04 becomes a pointer.
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

// Callers pass "ui/hud.mgb" and FUN_10554FC0 expands it to ui\localized\<branch>\<lang>\ui\, so only
// the leaf can be compared.
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
            // A reload hands back different pages, so what was learned about the old ones is stale.
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

// The frame the engine composes into, not the window: it is fitted to the output whole, so a frame
// shaped unlike the window is letterboxed. FUN_1032D910 describes the window and is the fallback.
static bool GetFrameAspect(float& fAspect)
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

    // The 1x1 stand-in reports aspect 1.0 and would read as a square display.
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

// Which authored layout the engine handed us: 4:3 is 1024x768, widescreen 1280x800. Only the edge
// offsets care; the canvas maths is the same either way.
static bool IsWidescreenAuthored(const Canvas& authored)
{
    return authored.nWidth * 3 > authored.nHeight * 4;
}


// Off the Package, not the context: the hook writes the context, so reading it back compounds the
// correction on a page that reaches BeginScreen twice.
static Canvas ReadAuthoredCanvas(const uint8_t* pPackage)
{
    Canvas authored;
    authored.nWidth = *reinterpret_cast<const uint16_t*>(pPackage + nPackageCanvasWidth);
    authored.nHeight = *reinterpret_cast<const uint16_t*>(pPackage + nPackageCanvasHeight);
    authored.nOriginX = *reinterpret_cast<const uint16_t*>(pPackage + nPackageOriginX);
    authored.nOriginY = *reinterpret_cast<const uint16_t*>(pPackage + nPackageOriginY);
    return authored;
}

// Height is the only difference between the HUD's canvas and a menu's. Width follows from height
// and aspect either way, which keeps px/unit equal on both axes.
static bool BuildPageCanvas(const Canvas& authored, float fAspect, PageKind kind, Canvas& fixed)
{
    const int nContentWidth = authored.nWidth - 2 * authored.nOriginX;
    const int nContentHeight = authored.nHeight - 2 * authored.nOriginY;

    if (nContentWidth <= 0 || nContentHeight <= 0)
        return false;

    if (!std::isfinite(fAspect) || fAspect <= 0.0f)
        return false;

    // Menus keep the authored height, so only the stretch goes. The HUD's comes off the content box
    // rather than the authored canvas, which keeps it at 696 for every aspect: the engine swaps the
    // authored canvas between 1280x800 and 1024x768, but both hold the same 960x720 of content.
    int nHeight = (kind == PageKind::GameplayHud)
        ? (nContentHeight * nCanvasHeightNumerator + nCanvasHeightDenominator / 2) / nCanvasHeightDenominator
        : authored.nHeight;

    int nWidth = static_cast<int>(nHeight * fAspect + 0.5f);

    // Fitting height alone breaks once the frame is narrower than the content box: a 1:1 frame
    // leaves 768 units of canvas for 960 units of content and the edges run off both sides. Below
    // that, fit the box instead and let the page scale down uniformly.
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

    // Not clamped to zero: the HUD canvas is shorter than its content box and the HUD is authored
    // past the box on every side, so a negative origin is correct and both ends read it as int16.
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

// The authored sizes double as the backdrop test, so they stay extents rather than fold into the
// scales.
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

// Widget::GetPackage. Through the vtable, since neither caller has a static call site to match.
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

// The reticle's marks spread as the weapon sways, so its origin walks up and down the frame;
// jumping or a sniper rifle took it over the top boundary and threw the top mark 62 units inward.
// Nothing this small and this central belongs to a corner cluster. 150 clears the marks at 459..484
// and stops short of the ammo at 981.
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

// Which page the offsets belong to, decided by content: an element reaching past the content box
// and narrower than it. Both halves matter -- stack order cannot separate the pages (the HUD is not
// drawn while the tutorial pop-up is up), and reaching past the box alone catches every backdrop.
// With the pop-up open the HUD had 22 of 32 quads outside the box at widths 0..256, the pop-up 4 of
// 1468 at 1280 and wider.
static bool IsAnchoredElement(float fOriginX, float fWidth)
{
    const float fBox = fContentCentre * 2.0f;
    if (fBox <= 0.0f)
        return false;

    if (fOriginX >= -fOutsideBoxMargin && fOriginX <= fBox + fOutsideBoxMargin)
        return false;

    return fWidth < fBox;
}

// 0x10AB5AEC, in FUN_10AB59A0's per-page block. ESI the magma::Package, EBX the render context, EDI
// the display stack, EBP the page index.
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

    // Those pixel counts in corrected-canvas units. The canvas is fitted to frame height, so one
    // unit is fixed.nHeight / 720 of a pixel at any canvas shape.
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

// A quad covering the authored canvas is the canvas, so it is remapped onto the real one instead of
// scaling with the content:
//
//     x' = (x + authoredOriginX - originX) * canvasWidth  / authoredCanvasWidth
//     y' = (y + authoredOriginY - originY) * canvasHeight / authoredCanvasHeight
//
// Per axis, since they do not arrive together -- the console strip spans the width and half the
// height. Covering, not merely large: the tutorial vignette is authored 1536x1024 to over-scan on
// purpose, and the widest ordinary content is a subtitle at about 784 units.
static bool IsBackdropAxis(float fExtent, float fAuthoredExtent)
{
    return fAuthoredExtent > 0.0f && fExtent >= fAuthoredExtent;
}

// 0x105FAD67, between the last vertex write and the divide. ESI is magma::Render.
//
// The quad rather than the widget: magma::Area carries a position and no extent, so a group cannot
// be measured from its own state. Every quad of one element shares the Area translation, so
// classifying on that cannot split a line of text.
static void ShiftQuad(SafetyHookContext& regs)
{
    if (!bPageCorrected)
        return;

    auto* pRender = reinterpret_cast<uint8_t*>(regs.esi);

    const float fGroupX = *reinterpret_cast<float*>(pRender + nRenderMatrixTranslateX);
    const float fGroupY = *reinterpret_cast<float*>(pRender + nRenderMatrixTranslateY);

    // Page origin taken back out, so the extent is in the same units as the origins.
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

    // Latched, so only quads ahead of the first anchored element on a page's first frame miss.
    if (!bAnchorPage && bGameplayHudPage && IsAnchoredElement(fGroupX, fRight - fLeft))
    {
        RememberAnchorPage(pCurrentPage);
        bAnchorPage = true;
    }

    const bool bBackdropX = IsBackdropAxis(fRight - fLeft, fAuthoredCanvasWidth);
    const bool bBackdropY = IsBackdropAxis(fBottom - fTop, fAuthoredCanvasHeight);
    if (bBackdropX || bBackdropY)
    {
        // Drawn before anything else on its page, so the flag is up when the console's quads arrive.
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
        // Strip page: all of it moves, version line to the right edge, everything else to the left.
        // Any other overlay page, only the top left corner.
        if (bConsoleStripPage)
            Move(pRender, nRenderVertexX, (fGroupX > fOverlayRightZone) ? fOverlayDeltaRight : fOverlayDeltaLeft);
        else if (fRight <= fCornerMaxX && fBottom <= fCornerMaxY)
            Move(pRender, nRenderVertexX, fOverlayDeltaLeft);

        return;
    }

    // Menus and dialogs stop here; below is the gameplay HUD's edge offsets.
    if (!bGameplayHudPage || !bAnchorPage)
        return;

    if (IsCentredDetail(fGroupX, fRight - fLeft, fBottom - fTop))
        return;

    int nBandX = BandOf(fGroupX, static_cast<float>(nLeftZoneMax), static_cast<float>(nRightZoneMin));

    // The 4:3 health bar, anchored past the boundary and only in its own row.
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

// 0x10AB5B7E. FUN_10AB59A0 draws the cursor after the page loop, ESI the topmost page's
// magma::Package, EBP the display: [EBP+0x34]/[EBP+0x36] canvas width/height, then vtable+0x24
// BeginScreen at 10ab5ba8. Last write to those each frame, and FUN_104EFAD0 clamps the cursor
// against them, so without this the cursor is held to the authored canvas. It never reaches the
// canvas hook, so the page flags are cleared here or the cursor quad hits the HUD band tests.
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

    // Width and height only: the origins this block writes are zero by design, the cursor living in
    // raw canvas units.
    auto* pContext = reinterpret_cast<uint8_t*>(regs.ebp);
    *reinterpret_cast<int16_t*>(pContext + nContextCanvasWidth) = static_cast<int16_t>(fixed.nWidth);
    *reinterpret_cast<int16_t*>(pContext + nContextCanvasHeight) = static_cast<int16_t>(fixed.nHeight);
}

// The click boxes

// 0x10AA16C0. __thiscall with RET 4, so __fastcall with an unused EDX is the same frame. magma hit
// tests in content units and converts the event here, reading the origin off the Package through
// vtable +0x44/+0x48 -- the authored one, while rendering uses the corrected one. Every hit box sat
// 22 units left and 52 up at 16:9, worse as the frame widens.
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

    // From this page's own package: input order between pages is not dependable and packages need
    // not share an authored canvas.
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

            // 0x105355B0, prologue through the empty-string proxy and the 0xF small-string capacity.
            auto lookupPattern = dunia_pattern(
                "81 EC 9C 00 00 00 53 56 57 8D 44 24 0F 50 8D 4C 24 34 33 DB BF ? ? ? ? BE 0F 00 00 00 51");

            // FUN_10AB59A0's per-page block. Hook site 0x10AB5AEC, thirteen bytes in.
            auto beginScreenPattern = dunia_pattern("0F B7 4C 24 10 66 89 43 38 66 89 4B 3A 8B 0D");

            // FUN_105FAB60's tail. Hook site 0x105FAD67, eighteen bytes in, after the last vertex
            // write so all four are readable. safetyhook's Context32 carries xmm0..xmm7, so a mid
            // hook inside this floating point run restores what it interrupted.
            auto quadPattern = dunia_pattern(
                "F3 0F 10 0D ? ? ? ? F3 0F 11 46 4C F3 0F 11 56 54 F3 0F 10 96 F0 00 00 00");

            // FUN_10AB59A0's cursor block. Hook site 0x10AB5B7E, seventeen bytes in; the word stores
            // to EBP tell it from the loop, which stores to EBX.
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

            // Independent of the two above and of each other, so a miss costs only its own fix.
            if (!cursorPattern.empty())
            {
                static auto CursorCanvasHook = safetyhook::create_mid(cursorPattern.get_first(0x11), ApplyCursorCanvas);
            }

            if (!eventPattern.empty())
                PageEventToContentHook = safetyhook::create_inline(eventPattern.get_first(0), PageEventToContent);
        };
    }
} HudFixes;
