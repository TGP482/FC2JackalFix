module;

#include <common.hxx>
#include <cstdint>

export module hudfixes;

import common;
import dunia;
import internalres;

// ---------------------------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------------------------

// magma::Package, through vtable +0x3C..+0x48.
static constexpr ptrdiff_t nPackageCanvasWidth = 0x54;
static constexpr ptrdiff_t nPackageCanvasHeight = 0x56;
static constexpr ptrdiff_t nPackageOriginX = 0x58;
static constexpr ptrdiff_t nPackageOriginY = 0x5A;

// Render context FUN_10AB59A0 fills before BeginScreen. FUN_105FA9C0 reads it back as signed shorts
// into Render+0xD8/0xD4/0xE0/0xE4.
static constexpr ptrdiff_t nContextCanvasWidth = 0x34;
static constexpr ptrdiff_t nContextCanvasHeight = 0x36;
static constexpr ptrdiff_t nContextOriginX = 0x38;
static constexpr ptrdiff_t nContextOriginY = 0x3A;

// magma::Render, the four quad vertices just before the divide, page origin already folded in:
//
//     105fac2f   LEA EDI,[ESI+0x04]     v0.x        v0.y at ESI+0x08
//     105fac6f   LEA EBX,[ESI+0x1c]     v1.x        v1.y at ESI+0x20
//     105facb7   LEA EBP,[ESI+0x34]     v2.x        v2.y at ESI+0x38
//     105fad03   LEA EAX,[ESI+0x4c]     v3.x        v3.y at ESI+0x50
static constexpr ptrdiff_t nRenderVertexX[4] = { 0x04, 0x1C, 0x34, 0x4C };
static constexpr ptrdiff_t nRenderVertexY[4] = { 0x08, 0x20, 0x38, 0x50 };
static constexpr ptrdiff_t nRenderOriginX = 0xE0;
static constexpr ptrdiff_t nRenderOriginY = 0xE4;

// Translation of the Area transform at Render+0x80. FUN_10065130 is column major, so m[12..14] is
// +0xB0/+0xB4. Every quad of one element shares it, which is what makes it usable as an identity.
static constexpr ptrdiff_t nRenderMatrixTranslateX = 0xB0;
static constexpr ptrdiff_t nRenderMatrixTranslateY = 0xB4;

// magma's display stack, EDI at the canvas hook:
//
//     10ab5af9   MOV ECX,[EDI+0x4]        the page vector
//     10ab5afc   MOV ECX,[ECX+EBP*0x8]    the page, so EBP is the index
static constexpr ptrdiff_t nDisplayPageVector = 0x04;
static constexpr size_t nDisplayPageEntrySize = 8;

// Widget vtable +0x20, GetPackage. A plain getter, safe to call from a hook.
static constexpr size_t nWidgetVtableGetPackage = 0x20 / sizeof(void*);

// Display descriptor from FUN_1032D910. A static 1x1 stand-in until a device exists.
static constexpr ptrdiff_t nDisplayValid = 0x00;
static constexpr ptrdiff_t nDisplayWidth = 0x08;
static constexpr ptrdiff_t nDisplayHeight = 0x0C;
static constexpr ptrdiff_t nDisplayAspect = 0x10;

// HUD canvas height, as a fraction of the authored content height. Console renders 1280x696 inside a
// 720p signal, so its HUD is sized against 696 of the 720 unit box; mapping the box to the full 720
// drew it 720/696 too small. Segment pitch and strip width off a Xenia capture both land on 696.
static constexpr int nCanvasHeightNumerator = 29;
static constexpr int nCanvasHeightDenominator = 30;

// The reticle is authored on the content box's centre, so the origin has to stay centred. An earlier
// -72 here dragged the health bar into place and took the reticle with it.
static constexpr int nContentOffsetY = 0;

// Element origin bands. Console runs banner to health bar in 466 px against this package's 571, so
// the top and bottom clusters move and the middle is left alone:
//
//     banner     authored y 118 .. 167     console 199    +62
//     reticle    authored y 317 .. 407     console 359      0
//     health     authored y 664 .. 725     console 661    -72
//
// Each boundary has to fall in a gap between whole elements, since anything landing inside one tears
// it in half. Known origins: banner backing -274.4, banner text 105, multiplayer crest 176 to 250,
// reticle and weapon swap 444 to 541.5, call icon 612 to 635 while it animates, then 663, 677, 682,
// and the ammo and health block 702 to 733.
//
// Top is 150. At 270 the multiplayer mode banner split, its crest taking the +62 and its text not.
//
// The bottom boundary is per column, because each column's cluster starts at a different height.
// Left is 400, below the diamond pickup pair at 548 and 580 so they move with the health bar. Centre
// is 620, clear of the weapon swap prompt. Right is 600, below the call icon's lowest origin.
static constexpr int nTopZoneMax = 150;
static constexpr int nBottomZoneMinLeft = 400;
static constexpr int nBottomZoneMin = 620;
static constexpr int nBottomZoneMinRight = 600;

// The diamond block rides the left column but lands 2 px right and 4 px high of console once the
// column shift is applied, measured on the icon's own edges rather than the digits. Bounded above by
// the syringes at 620, which are correct on the column shift alone.
static constexpr int nDiamondZoneMax = 620;
static constexpr int nDiamondShiftX = -2;
static constexpr int nDiamondShiftY = 4;

// Same case in the right column: the call icon sits 7 px, or 6.8 units, in from where console puts
// it. The bound is 650 rather than 692 because the grenade's origin is 663 and a wider bound threw
// it right along with the icon.
static constexpr int nCallIconZoneMax = 650;
static constexpr int nCallIconShiftX = 5;
static constexpr int nCallIconShiftY = -4;

// The icon is not scaled. Its height read 117 px on console against 112 here, but correlating the
// whole icon put a 117/112 scale 5.5% large, so the height reads had caught different edges.
static constexpr int nShiftTop = 62;
static constexpr int nShiftBottom = -72;

// The bottom corners disagree. With the health bar on console's rows, the ammo block still measured
// 3 to 4 px low across three glyphs at matching heights, so it is placement and not scale.
static constexpr int nShiftBottomRight = -75;

// Canvas width at which each side already sits where console puts it. The shift is half the
// difference from the real canvas, so originX plus shift is constant at every width, which is what
// makes it aspect independent.
//
//     strip left edge     authored -136.8   console 101    design width 1436
//     rightmost pixel     authored 1098.8   console 1180   design width 1438
//
// The right was 1437 until the ammo cluster was compared glyph for glyph. "Rightmost HUD pixel" was
// measuring the phone on one capture and the grenade on the other; the grenade column is 16 px wide
// in both and reads +5 px, so 4.8 units and a design width of 1447.
static constexpr int nDesignWidthLeft = 1437;
static constexpr int nDesignWidthRight = 1447;

// Column boundaries. Origins land at 82, 100 and 482 for the banner, 459 to 484 for the reticle and
// 981 upward for the ammo, so left goes anywhere in (42, 82) and right in (482, 981). Left is 60 and
// not 0 because the health bar is five segment quads with origins from -140 to +42, and a boundary
// of 0 left the last one behind in the middle of the bar.
static constexpr int nLeftZoneMax = 60;
static constexpr int nRightZoneMin = 700;

// Canvas shorts are written and read back as int16.
static constexpr int nCanvasCeiling = 32767;

// ---------------------------------------------------------------------------------------------
// Engine entry points
// ---------------------------------------------------------------------------------------------

// 0x1032D910. No arguments, result in EAX.
using GetDisplayDescriptor_t = uint8_t* (__cdecl*)();

// 0x105355B0. __stdcall, MSVC8 std::string by pointer, returns the magma::Package or null.
using GetPackageForPath_t = void* (__stdcall*)(void*);

static GetDisplayDescriptor_t GetDisplayDescriptor = nullptr;
static SafetyHookInline GetPackageForPathHook{};

// Only the gameplay HUD is named, and only because the edge offsets are a table measured against its
// authored layout. The canvas and the backdrop remap are not package specific, so naming pages for
// those meant a list that was always missing an entry: hud.mgb alone left the tutorial pop-up
// stretched, and adding common.mgb still left every menu package behind.
static constexpr const char* pszGameplayHudPackages[] =
{
    "hud.mgb",
    "hud_mp.mgb",
};
static constexpr int nGameplayHudPackages = static_cast<int>(sizeof(pszGameplayHudPackages) / sizeof(pszGameplayHudPackages[0]));

// Filled by the lookup hook, null until the HUD is asked for once. Null only stops the offsets.
static void* pGameplayHudPackages[nGameplayHudPackages] = {};

// The pages of that package the offsets apply to, learned from their contents. One in practice. Kept
// here because the lookup hook has to empty it on a reload.
static constexpr int nMaxAnchorPages = 8;
static void* pAnchorPages[nMaxAnchorPages] = {};
static int nAnchorPages = 0;

// ---------------------------------------------------------------------------------------------
// Identifying the packages
// ---------------------------------------------------------------------------------------------

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
// the leaf can be compared. Returns the slot so a reload of one package is not read as another.
static int GameplayHudSlot(const char* pszPath)
{
    const char* pLeaf = pszPath;
    for (const char* p = pszPath; *p != '\0'; p++)
    {
        if (*p == '/' || *p == '\\')
            pLeaf = p + 1;
    }

    for (int i = 0; i < nGameplayHudPackages; i++)
    {
        if (_stricmp(pLeaf, pszGameplayHudPackages[i]) == 0)
            return i;
    }

    return -1;
}

static bool IsGameplayHudPackage(const void* pPackage)
{
    if (pPackage == nullptr)
        return false;

    for (int i = 0; i < nGameplayHudPackages; i++)
    {
        if (pGameplayHudPackages[i] == pPackage)
            return true;
    }

    return false;
}

static void* __stdcall GetPackageForPath(void* pPath)
{
    auto* pPackage = GetPackageForPathHook.stdcall<void*>(pPath);

    char szPath[MAX_PATH];
    ReadDuniaString(reinterpret_cast<uintptr_t>(pPath), szPath, sizeof(szPath));

    const int nSlot = (szPath[0] != '\0') ? GameplayHudSlot(szPath) : -1;

    if (pPackage != nullptr && nSlot >= 0 && pGameplayHudPackages[nSlot] != pPackage)
    {
        // A reload hands back different pages, so anything learned about the old ones is stale.
        pGameplayHudPackages[nSlot] = pPackage;
        nAnchorPages = 0;
    }

    return pPackage;
}

// ---------------------------------------------------------------------------------------------
// The canvas
// ---------------------------------------------------------------------------------------------

enum class PageKind { GameplayHud, Menu };

static PageKind PageKindOf(const void* pPackage)
{
    return IsGameplayHudPackage(pPackage) ? PageKind::GameplayHud : PageKind::Menu;
}

// The frame the engine composes into, not the window. It is fitted to the output whole, so a frame
// shaped unlike the window is letterboxed and everything in it keeps the frame's shape.
// FUN_1032D910 describes the window and is only the fallback.
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

    // The 1x1 stand-in reports aspect 1.0 and would be taken for a square display.
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

// Off the Package, not the context. The context is what the canvas hook writes, so reading it back
// compounds the correction on a page that reaches BeginScreen twice.
static Canvas ReadAuthoredCanvas(const uint8_t* pPackage)
{
    Canvas authored;
    authored.nWidth = *reinterpret_cast<const uint16_t*>(pPackage + nPackageCanvasWidth);
    authored.nHeight = *reinterpret_cast<const uint16_t*>(pPackage + nPackageCanvasHeight);
    authored.nOriginX = *reinterpret_cast<const uint16_t*>(pPackage + nPackageOriginX);
    authored.nOriginY = *reinterpret_cast<const uint16_t*>(pPackage + nPackageOriginY);
    return authored;
}

// Height is the only difference between the HUD's canvas and a menu's. Width follows from height and
// aspect either way, which is what makes px/unit equal on both axes.
static bool BuildPageCanvas(const Canvas& authored, float fAspect, PageKind kind, Canvas& fixed)
{
    const int nContentWidth = authored.nWidth - 2 * authored.nOriginX;
    const int nContentHeight = authored.nHeight - 2 * authored.nOriginY;

    if (nContentWidth <= 0 || nContentHeight <= 0)
        return false;

    if (!std::isfinite(fAspect) || fAspect <= 0.0f)
        return false;

    // Menus keep the authored height, so their vertical is untouched and only the stretch goes.
    const int nHeight = (kind == PageKind::GameplayHud)
        ? (nContentHeight * nCanvasHeightNumerator + nCanvasHeightDenominator / 2) / nCanvasHeightDenominator
        : authored.nHeight;

    const int nWidth = static_cast<int>(nHeight * fAspect + 0.5f);

    if (nWidth < 1 || nHeight < 1 || nWidth > nCanvasCeiling || nHeight > nCanvasCeiling)
        return false;

    fixed.nWidth = nWidth;
    fixed.nHeight = nHeight;
    fixed.nOriginX = (nWidth - nContentWidth) / 2;

    // Not clamped to zero. The HUD canvas is shorter than its content box and the HUD is authored
    // past the box on every side, so a negative origin is correct and both ends read it as int16.
    fixed.nOriginY = (nHeight - nContentHeight) / 2 + nContentOffsetY;
    return true;
}

// ---------------------------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------------------------

static bool bPageCorrected = false;
static bool bGameplayHudPage = false;
static bool bAnchorPage = false;

// The authored sizes double as the backdrop test, so they stay as extents rather than folding into
// the scales.
static float fAuthoredCanvasWidth = 0.0f;
static float fAuthoredCanvasHeight = 0.0f;
static float fBackdropOffsetX = 0.0f;
static float fBackdropOffsetY = 0.0f;
static float fBackdropScaleX = 1.0f;
static float fBackdropScaleY = 1.0f;

static float fContentCentre = 0.0f;
static float fDeltaLeft = 0.0f;
static float fDeltaRight = 0.0f;

// Widget::GetPackage. Through the vtable because neither caller has a static call site to match.
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

// The reticle's marks spread apart as the weapon sways, so its origin walks up and down the frame.
// Jumping or holding a sniper rifle took it across the top boundary and threw the top mark 62 units
// into the middle of the screen. Nothing this small and this close to the centre is part of a corner
// cluster. 150 clears the marks at 459 to 484 and stops short of the ammo at 981.
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

// Which page the offsets belong to, decided by what is authored on it: an element reaching past the
// content box, and narrower than it. Both halves matter. Stack order cannot tell the two pages apart
// because the HUD is not drawn at all while the tutorial pop-up is up, and reaching past the box
// alone catches every backdrop. In a frame with the pop-up open the HUD had 22 of 32 quads outside
// the box at widths 0 to 256, the pop-up 4 of 1468 at 1280 and wider.
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
    bGameplayHudPage = IsGameplayHudPackage(pPackage);
    bAnchorPage = false;

    if (!bPageCorrected)
        return;

    float fAspect = 0.0f;
    if (!GetFrameAspect(fAspect))
    {
        bPageCorrected = false;
        bGameplayHudPage = false;
        return;
    }

    const Canvas authored = ReadAuthoredCanvas(pPackage);

    Canvas fixed;
    if (!BuildPageCanvas(authored, fAspect, PageKindOf(pPackage), fixed))
    {
        bPageCorrected = false;
        bGameplayHudPage = false;
        return;
    }

    auto* pContext = reinterpret_cast<uint8_t*>(regs.ebx);
    *reinterpret_cast<int16_t*>(pContext + nContextCanvasWidth) = static_cast<int16_t>(fixed.nWidth);
    *reinterpret_cast<int16_t*>(pContext + nContextCanvasHeight) = static_cast<int16_t>(fixed.nHeight);
    *reinterpret_cast<int16_t*>(pContext + nContextOriginX) = static_cast<int16_t>(fixed.nOriginX);
    *reinterpret_cast<int16_t*>(pContext + nContextOriginY) = static_cast<int16_t>(fixed.nOriginY);

    fContentCentre = (authored.nWidth - 2 * authored.nOriginX) * 0.5f;
    fDeltaLeft = -(fixed.nWidth - nDesignWidthLeft) * 0.5f;
    fDeltaRight = (fixed.nWidth - nDesignWidthRight) * 0.5f;

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

// A quad covering the authored canvas is the canvas, not a large element, so it is remapped onto the
// real one instead of scaling with the content:
//
//     x' = (x + authoredOriginX - originX) * canvasWidth  / authoredCanvasWidth
//     y' = (y + authoredOriginY - originY) * canvasHeight / authoredCanvasHeight
//
// Per axis, because they do not arrive together: the developer console spans the width and a third
// of the height, and a test wanting both left the canvas margins beside it uncovered. Covering, not
// merely large, because the tutorial pop-up's vignette is authored 1536 x 1024 to over-scan on
// purpose and has to keep doing so. The widest content on any page is a subtitle line at about 784
// units against a 1280 canvas, so the threshold is not close to anything.
static bool IsBackdropAxis(float fExtent, float fAuthoredExtent)
{
    return fAuthoredExtent > 0.0f && fExtent >= fAuthoredExtent;
}

// 0x105FAD67, between the last vertex write and the divide. ESI is magma::Render.
//
// The quad rather than the widget, because magma::Area carries a position and no extent, so a group
// cannot be measured from its own state, and walking to the leaves classified 44 individual glyphs
// including the reticle's marks. Every drawable reaches FUN_105FAB60, and every quad of one element
// shares the Area translation, so classifying on that cannot split a line of text.
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

    // Latched, so only the quads ahead of the first anchored element on a page's first frame miss.
    if (!bAnchorPage && bGameplayHudPage && IsAnchoredElement(fGroupX, fRight - fLeft))
    {
        RememberAnchorPage(pCurrentPage);
        bAnchorPage = true;
    }

    const bool bBackdropX = IsBackdropAxis(fRight - fLeft, fAuthoredCanvasWidth);
    const bool bBackdropY = IsBackdropAxis(fBottom - fTop, fAuthoredCanvasHeight);
    if (bBackdropX || bBackdropY)
    {
        if (bBackdropX)
            Remap(pRender, nRenderVertexX, fBackdropOffsetX, fBackdropScaleX);
        if (bBackdropY)
            Remap(pRender, nRenderVertexY, fBackdropOffsetY, fBackdropScaleY);
        return;
    }

    // Menus and dialogs stop here. Everything below is the gameplay HUD's edge offsets.
    if (!bGameplayHudPage || !bAnchorPage)
        return;

    if (IsCentredDetail(fGroupX, fRight - fLeft, fBottom - fTop))
        return;

    const int nBandX = BandOf(fGroupX, static_cast<float>(nLeftZoneMax), static_cast<float>(nRightZoneMin));

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

// ---------------------------------------------------------------------------------------------
// The cursor
// ---------------------------------------------------------------------------------------------

// 0x10AB5B7E. FUN_10AB59A0 draws the cursor after the page loop, in a block of its own, with ESI the
// topmost page's magma::Package and EBP the display:
//
//     10ab5b76   MOV [EBP+0x34],AX        canvas width
//     10ab5b7a   MOV [EBP+0x36],BX        canvas height
//     10ab5ba8   CALL [vtable+0x24]       BeginScreen, then FUN_10AA21A0 per cursor slot
//
// This block is the last thing to write those two each frame, and FUN_104EFAD0 clamps the cursor's
// accumulated position against them, so without it the cursor was clamped to the authored canvas
// while everything else drew against the corrected one. It also never reaches the canvas hook, so
// the page flags were left over from the loop and the cursor quad went through the HUD's band
// classification, jumping sideways as its origin crossed a boundary.
static void ApplyCursorCanvas(SafetyHookContext& regs)
{
    // Not a page of the loop, so nothing here may be shifted.
    bPageCorrected = false;
    bGameplayHudPage = false;
    bAnchorPage = false;

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

    // Width and height only. The origins this block writes are zero by design, since the cursor
    // lives in raw canvas units.
    auto* pContext = reinterpret_cast<uint8_t*>(regs.ebp);
    *reinterpret_cast<int16_t*>(pContext + nContextCanvasWidth) = static_cast<int16_t>(fixed.nWidth);
    *reinterpret_cast<int16_t*>(pContext + nContextCanvasHeight) = static_cast<int16_t>(fixed.nHeight);
}

// ---------------------------------------------------------------------------------------------
// The click boxes
// ---------------------------------------------------------------------------------------------

// 0x10AA16C0. __thiscall with RET 4, so __fastcall with an unused EDX is the same frame.
//
// magma hit tests in content units, and this is where the event is converted. It reads the origin
// off the Package through vtable +0x44 and +0x48, which is the authored one, while rendering uses
// the corrected one, so every hit box sat 22 units left and 52 up at 16:9 and further out as the
// frame widens.
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

    // From this page's own package, since input does not arrive between pages in any order this
    // could rely on and the packages need not share an authored canvas.
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

            // 0x105355B0, prologue as far as the empty-string proxy and the 0xF small-string
            // capacity behind it.
            auto lookupPattern = dunia_pattern(
                "81 EC 9C 00 00 00 53 56 57 8D 44 24 0F 50 8D 4C 24 34 33 DB BF ? ? ? ? BE 0F 00 00 00 51");

            // FUN_10AB59A0's per-page block. Hook site is 0x10AB5AEC, thirteen bytes in.
            auto beginScreenPattern = dunia_pattern("0F B7 4C 24 10 66 89 43 38 66 89 4B 3A 8B 0D");

            // FUN_105FAB60's tail. Hook site is 0x105FAD67, eighteen bytes in, after the last vertex
            // write so all four are readable. safetyhook's Context32 carries xmm0..xmm7, so a mid
            // hook inside this floating point run restores what it interrupted.
            auto quadPattern = dunia_pattern(
                "F3 0F 10 0D ? ? ? ? F3 0F 11 46 4C F3 0F 11 56 54 F3 0F 10 96 F0 00 00 00");

            // FUN_10AB59A0's cursor block. Hook site is 0x10AB5B7E, seventeen bytes in. The word
            // stores to EBP are what tell it from the loop, which stores to EBX.
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
