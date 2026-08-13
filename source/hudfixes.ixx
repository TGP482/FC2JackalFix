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

// magma::Package, the four shorts the draw loop reads through vtable +0x3C..+0x48.
static constexpr ptrdiff_t nPackageCanvasWidth = 0x54;
static constexpr ptrdiff_t nPackageCanvasHeight = 0x56;
static constexpr ptrdiff_t nPackageOriginX = 0x58;
static constexpr ptrdiff_t nPackageOriginY = 0x5A;

// The render context FUN_10AB59A0 fills before BeginScreen. Same four values, same order, and
// FUN_105FA9C0 reads them back as signed shorts into Render+0xD8/0xD4/0xE0/0xE4.
static constexpr ptrdiff_t nContextCanvasWidth = 0x34;
static constexpr ptrdiff_t nContextCanvasHeight = 0x36;
static constexpr ptrdiff_t nContextOriginX = 0x38;
static constexpr ptrdiff_t nContextOriginY = 0x3A;

// magma::Render. The four quad vertices as FUN_105FAB60 leaves them just before the divide, and the
// page origin it has already added to each axis:
//
//     105fac2f   LEA EDI,[ESI+0x04]     v0.x        v0.y at ESI+0x08
//     105fac6f   LEA EBX,[ESI+0x1c]     v1.x        v1.y at ESI+0x20
//     105facb7   LEA EBP,[ESI+0x34]     v2.x        v2.y at ESI+0x38
//     105fad03   LEA EAX,[ESI+0x4c]     v3.x        v3.y at ESI+0x50
static constexpr ptrdiff_t nRenderVertexX[4] = { 0x04, 0x1C, 0x34, 0x4C };
static constexpr ptrdiff_t nRenderVertexY[4] = { 0x08, 0x20, 0x38, 0x50 };
static constexpr ptrdiff_t nRenderOriginX = 0xE0;
static constexpr ptrdiff_t nRenderOriginY = 0xE4;

// The Area transform magma::Render applies to each quad, at Render+0x80. FUN_10065130 multiplies as
// out.x = m[0]*x + m[4]*y + m[8]*z + m[12], so the matrix is column major and the translation sits
// at m[12..14] - Render+0xB0 and +0xB4. Every quad of one element shares it.
static constexpr ptrdiff_t nRenderMatrixTranslateX = 0xB0;
static constexpr ptrdiff_t nRenderMatrixTranslateY = 0xB4;

// magma's display stack, which is FUN_10AB59A0's argument and sits in EDI at the canvas hook:
//
//     10ab5af9   MOV ECX,[EDI+0x4]        the page vector
//     10ab5afc   MOV ECX,[ECX+EBP*0x8]    the page, so EBP is the index
static constexpr ptrdiff_t nDisplayPageVector = 0x04;
static constexpr size_t nDisplayPageEntrySize = 8;

// Widget vtable +0x20, GetPackage. FUN_10AB59A0 calls it once per page to fetch the canvas and
// FUN_10AA16C0 calls it again to fetch the origin, so it is a plain getter and safe from a hook.
static constexpr size_t nWidgetVtableGetPackage = 0x20 / sizeof(void*);

// The display descriptor FUN_1032D910 returns, which is DAT_11609668+0x14 once a device exists and
// a static 1x1 stand-in with aspect 1.0 before that.
static constexpr ptrdiff_t nDisplayValid = 0x00;
static constexpr ptrdiff_t nDisplayWidth = 0x08;
static constexpr ptrdiff_t nDisplayHeight = 0x0C;
static constexpr ptrdiff_t nDisplayAspect = 0x10;

// Gameplay HUD canvas height as a multiple of the authored content height, which is what sets the
// scale: px/unit is frameHeight/canvasHeight, so a smaller canvas draws a bigger HUD.
//
// 29/30 is 696 of the 720 unit content box, which is console's picture height rather than its frame
// height. Console renders 1280x696 inside a 720p signal, so its HUD is sized against 696; the PC
// target is a full 1280x720 with the twelve pixel bars drawn into it, so mapping the content box to
// the whole 720 drew everything 720/696 too small. Three measurements against a Xenia capture at the
// same frame size agree:
//
//     segment pitch      console 48.0 px    PC 46.5     720 x 46.5/48.0 = 697.5
//     strip width        console 237 px     PC 229      720 x 229/237   = 695.7
//     console segments   (101,145) (149,193) (197,241) (245,289) (293,337)
//
// It lands the strip's left edge on 101, the rightmost HUD pixel on 1180, the pitch on 48.1 and the
// strip's row on 661 - all four within a pixel of console - with the design widths and the vertical
// offset left where they already were, which is what says it is the right constant and not a fit.
//
// The gameplay HUD only. Every other page keeps its authored height; this number was measured
// against the health bar and is the HUD's own overscan, not a property of the library.
static constexpr int nCanvasHeightNumerator = 29;
static constexpr int nCanvasHeightDenominator = 30;

// Centred, and it has to be. The reticle is authored at the content box's own centre, y 361.4 of
// 720, so a centred origin is what puts it on the middle of the screen. The -72 that stood here
// shifted the whole page up to drag the health bar into place and took the reticle with it.
static constexpr int nContentOffsetY = 0;

// Vertical anchoring, the same shape as the horizontal but with the boundaries measured rather than
// derived, because unlike the sides the vertical does not vary with the frame.
//
// The two layouts disagree on vertical spread: console runs banner to health bar in 466 px, the PC
// package in 571, at identical element size. No single origin satisfies both, so the top and bottom
// bands move and the middle is left alone:
//
//     element    authored y        console    shift
//     banner     118    .. 167     199        +62
//     reticle    317.0  .. 406.9   359          0
//     health     664.0  .. 724.9   661        -72
//
// The banner shift was 29 first, from a y read off a detector that had caught the sky along with the
// bar. Measured on the text body instead - a dense run of bright rows, and nothing else in the frame
// is - console puts it at rows 195..203 against 161..169 here, so 34 px short.
//
// 620 rather than 540 for the bottom boundary. Every element origin y the gameplay HUD has emitted
// across four dumps:
//
//     -274.4                                          the banner's backing
//     444.0  516.0  520.0  532.4  534.5  539.4  541.5  the reticle and the weapon swap prompt
//     702.0  712.0  713.0  715.0  720.0  729.0  733.0  the ammo block and the health bar
//
// 540 fell inside the weapon swap prompt at 532..541.5 and cut it in two: the arrow and one half of
// each icon stayed while the 541.5 half went up 72 and landed beside the button glyph. 620 sits in
// the middle of the gap, clearing the prompt by 78 and the ammo block by 82.
static constexpr int nTopZoneMax = 270;
static constexpr int nBottomZoneMin = 620;
static constexpr int nShiftTop = 62;
static constexpr int nShiftBottom = -72;

// The bottom band needs two values, because the two corners do not agree. With the health bar on
// console's rows exactly, the ammo block still measured 3 to 4 px lower across three glyphs, all at
// matching heights so it is placement and not scale:
//
//     "30" digits     console 646..662    PC 649..665    +3 px
//     bullets icon    console 645..662    PC 649..664    +4 px
//     grenade icon    console 601..636    PC 604..640    +3 px
//
// The right-hand corner takes 3.4 units more, which is the only place the vertical has to know about
// the horizontal.
static constexpr int nShiftBottomRight = -75;

// The canvas width at which each side already sits where console puts it. The shifts are half the
// difference between the real canvas and these, so originX plus the shift is a constant - 265 units
// in from the left, 1324 in from the right - at every canvas width, which is what makes them aspect
// independent.
//
//     strip left edge     authored -136.8   console 101    shift +77.8   design width 1436
//     rightmost pixel     authored 1098.8   console 1180   shift -78.8   design width 1438
//
// The right was 1437 with the left until the ammo cluster was compared glyph for glyph. "Rightmost
// HUD pixel" turned out to be measuring different artwork on the two captures - the PC shot had the
// phone in hand and console's did not, and the phone reaches further right than the grenade. The
// grenade and bullet column is the same art in both and reads 16 px wide in both, so it is an offset
// and not a scale:
//
//     grenade column     console 1161..1176    PC 1166..1181     +5 px
//     ammo digits, left  console 1131          PC 1135           +4 px
//
// 5 px is 4.8 units, so the right shift goes -100 to -104.8 and the design width to 1447.
static constexpr int nDesignWidthLeft = 1437;
static constexpr int nDesignWidthRight = 1447;

// Where the two anchored bands begin, tested against the element's origin rather than the quad.
//
// Per-quad classification could not survive text. A line of subtitle is about 45 separate quads and
// any boundary falling inside the line threw its ends to the screen edges. Two builds proved no
// boundary avoids it: the health bar trough stayed intact at 180 and tore at 96, and the subtitle's
// first glyph did the opposite, so both extents live inside (96, 180). y does not separate them
// either - the bar's authored y is 721..725 and the subtitle line's is 695..727.
//
// Every quad an element emits shares the Area matrix at Render+0x80, so Render+0xB0/+0xB4 is the
// element's own origin and all 45 glyphs of the banner's text report the identical 100,105.
// Classifying on that makes splitting an element structurally impossible. The boundaries then come
// straight out of the dump, as origins:
//
//     banner bar halves      82, 482      banner text  100      tall backing  355
//     reticle marks          459, 476, 484
//     ammo blocks            981, 988, 1026, 1073, 1075
//     health bar             not in that frame; its authored extent is -140..88, so about -140
//
// Left goes anywhere in (42, 82) and right anywhere in (482, 981).
//
// The left one is 60 and not 0 because an element is not one origin: the health bar is five segment
// quads spanning -140..88, so their origins sit at roughly -140, -94, -49, -3 and +42. A boundary of
// 0 put the first four in the left band and left the fifth behind, drawing it 103 px left of where
// it belongs, inside the middle of the bar. The boundary has to clear the whole run of an element's
// origins, not just its first.
static constexpr int nLeftZoneMax = 60;
static constexpr int nRightZoneMin = 700;

// A canvas short is written back as int16 and read back as int16.
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

// The canvas goes on every package and only the gameplay HUD is named.
//
// It was a list of package names once, and the list was the problem. hud.mgb alone left the plain
// tutorial pop-up stretched, because that one is built from the generic message box in common.mgb
// rather than from a hud.mgb page; adding common.mgb fixed it and, by the same accident, unstretched
// the load page and the quit box. Everything still outside the list - sp_pause_menus.mgb,
// sp_menus.mgb, mp_menus.mgb, weapon_bazaar.mgb, 360.mgb - stayed stretched, so at 32:9 a dialog sat
// correctly proportioned on top of a notebook that was not. None of the three corrections is
// package-specific, so no list can be short of an entry again.
//
// The edge offsets are the exception and stay named, because they are a table measured against one
// authored layout and mean nothing on a dialog or a menu.
static constexpr const char* pszGameplayHudPackages[] =
{
    "hud.mgb",
    "hud_mp.mgb",
};
static constexpr int nGameplayHudPackages = static_cast<int>(sizeof(pszGameplayHudPackages) / sizeof(pszGameplayHudPackages[0]));

// Written only by the lookup hook, read only by the draw hooks, and null until the HUD has been
// asked for once. Null only stops the offsets; the canvas does not depend on knowing a name.
static void* pGameplayHudPackages[nGameplayHudPackages] = {};

// The pages of that package the offsets are for, learned from their contents rather than named. One
// page in practice; the spare room is only so that a package with two of them would not silently
// lose the second. It lives here because the lookup hook has to empty it on a reload.
static constexpr int nMaxAnchorPages = 8;
static void* pAnchorPages[nMaxAnchorPages] = {};
static int nAnchorPages = 0;

// ---------------------------------------------------------------------------------------------
// Identifying the packages
// ---------------------------------------------------------------------------------------------

// MSVC8 std::string: size at +0x14, capacity at +0x18, buffer inline at +0x04 until it outgrows 16
// bytes and +0x04 becomes a pointer to it. cinematichud reads the same shape off a stack argument
// and this is duplicated rather than shared for the same reason it is duplicated there.
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

// Callers spell it "ui/hud.mgb"; FUN_10554FC0 turns that into ui\localized\<branch>\<lang>\ui\ before
// the file is opened. Only the leaf is compared, so either separator and either branch answer the
// same, and hud_mp.mgb does not answer to hud.mgb. Returns the slot, so a reload of one package
// cannot be mistaken for another arriving.
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

// The shape of the frame the engine composes into, which is the shape the interface is seen at.
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

    // The stand-in FUN_1032D910 hands back before a device exists is 1x1 with aspect 1.0, which
    // would otherwise be taken for a square display.
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

// Read off the Package, not off the context. The context is what the canvas hook writes, so reading
// it back would compound the correction on any page that reaches BeginScreen twice.
static Canvas ReadAuthoredCanvas(const uint8_t* pPackage)
{
    Canvas authored;
    authored.nWidth = *reinterpret_cast<const uint16_t*>(pPackage + nPackageCanvasWidth);
    authored.nHeight = *reinterpret_cast<const uint16_t*>(pPackage + nPackageCanvasHeight);
    authored.nOriginX = *reinterpret_cast<const uint16_t*>(pPackage + nPackageOriginX);
    authored.nOriginY = *reinterpret_cast<const uint16_t*>(pPackage + nPackageOriginY);
    return authored;
}

// The height is the whole difference between the two canvases this builds. Width follows from it and
// the frame's aspect either way, so canvasWidth/canvasHeight is the frame's ratio and px/unit comes
// out the same on both axes.
static bool BuildPageCanvas(const Canvas& authored, float fAspect, bool bGameplayHud, Canvas& fixed)
{
    const int nContentWidth = authored.nWidth - 2 * authored.nOriginX;
    const int nContentHeight = authored.nHeight - 2 * authored.nOriginY;

    if (nContentWidth <= 0 || nContentHeight <= 0)
        return false;

    if (!std::isfinite(fAspect) || fAspect <= 0.0f)
        return false;

    // Menus and dialogs keep the authored height, which leaves their vertical bit for bit what the
    // package asked for and takes the stretch out of the horizontal alone.
    const int nHeight = bGameplayHud
        ? (nContentHeight * nCanvasHeightNumerator + nCanvasHeightDenominator / 2) / nCanvasHeightDenominator
        : authored.nHeight;

    const int nWidth = static_cast<int>(nHeight * fAspect + 0.5f);

    if (nWidth < 1 || nHeight < 1 || nWidth > nCanvasCeiling || nHeight > nCanvasCeiling)
        return false;

    fixed.nWidth = nWidth;
    fixed.nHeight = nHeight;
    fixed.nOriginX = (nWidth - nContentWidth) / 2;

    // Not clamped to zero. The HUD canvas is shorter than the content box and the HUD is authored
    // past the box on every side, so both origins are legitimately negative; the context takes them
    // as signed shorts and FUN_105FA9C0 reads them back the same way. On a menu, where nHeight is
    // the authored height, this comes back out as the authored origin.
    fixed.nOriginY = (nHeight - nContentHeight) / 2 + nContentOffsetY;
    return true;
}

// ---------------------------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------------------------

// Set for a page whose canvas this rewrote, which is every page it can work one out for.
static bool bPageCorrected = false;

// Set for pages of the gameplay HUD package, which are the only ones allowed to become anchor pages.
static bool bGameplayHudPage = false;

// Set for the gameplay HUD page alone, which is the set of pages the offsets are for.
static bool bAnchorPage = false;

// What it takes to put a quad authored against the authored canvas onto the real one. The two
// authored sizes double as the test for whether a quad is one of those, so they are kept as extents
// rather than folded into the scales.
static float fAuthoredCanvasWidth = 0.0f;
static float fAuthoredCanvasHeight = 0.0f;
static float fBackdropOffsetX = 0.0f;
static float fBackdropOffsetY = 0.0f;
static float fBackdropScaleX = 1.0f;
static float fBackdropScaleY = 1.0f;

static float fContentCentre = 0.0f;
static float fDeltaLeft = 0.0f;
static float fDeltaRight = 0.0f;

// Widget::GetPackage, through the vtable rather than by pattern, because the draw loop and the hit
// test both reach it that way and neither has a static call site to match.
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

// The page the draw loop is on, read out of the vector EDI points at with the index in EBP.
static void* PageAt(uint8_t* pDisplay, uint32_t nIndex)
{
    auto* pEntries = *reinterpret_cast<uint8_t**>(pDisplay + nDisplayPageVector);
    if (pEntries == nullptr)
        return nullptr;

    return *reinterpret_cast<void**>(pEntries + nIndex * nDisplayPageEntrySize);
}

// The page currently being drawn, latched by the canvas hook for the quad hook to attribute to.
static void* pCurrentPage = nullptr;

// How far outside the content box an element has to be authored before it counts as anchored to a
// screen edge. The two clusters that qualify clear it comfortably - the health bar starts at -146 and
// the ammo block at 981 against a box of 0..960 - so the margin only keeps an element that merely
// touches an end from naming its page.
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

// An element anchored to a screen edge: authored past either end of the content box, and narrower
// than the box. Both halves are load bearing, and this is the third page test - the first two were
// stack order and the outside-the-box half alone.
//
// Stack order fails because the draw loop starts at display+0x10 rather than zero and a page that
// covers everything below it moves that index up. With the tutorial pop-up on screen the gameplay
// HUD is not drawn at all, so the pop-up is the first page of the package in the frame; ordering
// cannot separate two pages when only one of them is ever drawn.
//
// Outside the box alone fails on backdrops, which have to cover the screen and so are authored past
// both ends exactly as the health bar is. What the dump with a pop-up up says:
//
//     gameplay HUD      32 quads, 22 of them outside the box, widths 0 to 256
//     tutorial pop-up   1468 quads, 4 of them outside the box, widths 1280, 1320 and 1536
//
// The box itself is 960, so there is 704 units of clearance below and 320 above and no near miss on
// either side.
//
// Tightening the bands instead does not work, which is worth recording since it looks like the
// obvious move. The tip banner sits at y 105 and the pop-up's first line at about 115, so no y
// boundary separates them, and the pop-up's damage is entirely vertical: "Sniper Rifles" moves 251
// to 313 while "Assault Rifles" at 282 does not, which is one boundary falling between two rows of
// the same list. Its x origins are all inside the bands already.
static bool IsAnchoredElement(float fOriginX, float fWidth)
{
    const float fBox = fContentCentre * 2.0f;
    if (fBox <= 0.0f)
        return false;

    if (fOriginX >= -fOutsideBoxMargin && fOriginX <= fBox + fOutsideBoxMargin)
        return false;

    return fWidth < fBox;
}

// 0x10AB5AEC, in FUN_10AB59A0's per-page block. ESI is the magma::Package, EBX the render context,
// EDI the display stack and EBP the page index.
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
    if (!BuildPageCanvas(authored, fAspect, bGameplayHudPage, fixed))
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

    // A page this has not seen before is not an anchor page until one of its own quads says so,
    // which the quad hook does.
    pCurrentPage = PageAt(reinterpret_cast<uint8_t*>(regs.edi), static_cast<uint32_t>(regs.ebp));
    bAnchorPage = IsAnchorPage(pCurrentPage);
}

// Which band of one axis an origin falls in: -1 below the low boundary, +1 above the high one, 0
// between them.
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

// A quad that covers the whole authored canvas is not a large element, it is the canvas, and it has
// to be remapped onto the real one rather than scaled with the content:
//
//     x' = (x + authoredOriginX - originX) * canvasWidth  / authoredCanvasWidth
//     y' = (y + authoredOriginY - originY) * canvasHeight / authoredCanvasHeight
//
// The tutorial pop-up's backdrop is the case that found it. Authored at -160..1120 by -40..760 in
// content units, which is the 1280 x 800 canvas seen from an origin of 160, 40, it landed on 0..1280
// across and -52..748 down at a corrected canvas of 1280 x 696 - 800 pixels of backdrop hanging 52
// off each end of a 696 pixel frame.
//
// Both axes have to cover, not merely be large: the pop-up's widest row of text is 660 units, and
// its vignette, authored at 1536 x 1024 to over-scan on purpose, is carried out to -111..780 rather
// than clamped, so it keeps over-scanning by the proportion it was drawn with.
static bool IsBackdrop(float fWidth, float fHeight)
{
    if (fAuthoredCanvasWidth <= 0.0f || fAuthoredCanvasHeight <= 0.0f)
        return false;

    return fWidth >= fAuthoredCanvasWidth && fHeight >= fAuthoredCanvasHeight;
}

// 0x105FAD67, between the last vertex write and the divide. ESI is magma::Render and the four
// vertices are in canvas units with the page origin already folded into each axis.
//
// Shifting the quad rather than the widget, because the widget tree cannot do it. magma::Area
// carries a position and no extent - Area::Draw, FUN_10AB9CE0, reads State+0x24/+0x26 and calls
// MatrixStack::Translate, which is magma::PosState and not the RectState that Image::Draw reads as
// left/right/top/bottom - so a group cannot be measured from its own state. Walking to the leaves
// instead does not give groups either: almost nothing in this tree straddles the centre, so "the
// first subtree that does not" ran to 44 individual glyphs including the reticle's marks. And
// Area::Draw only runs for widgets that have a child area, so most of what the walk classified never
// reached it and the shifts were computed, logged and applied to nothing. Every drawable reaches
// FUN_105FAB60.
static void ShiftQuad(SafetyHookContext& regs)
{
    if (!bPageCorrected)
        return;

    auto* pRender = reinterpret_cast<uint8_t*>(regs.esi);

    const float fGroupX = *reinterpret_cast<float*>(pRender + nRenderMatrixTranslateX);
    const float fGroupY = *reinterpret_cast<float*>(pRender + nRenderMatrixTranslateY);

    // The quad's own extent, with the page origin taken back out so it is in the same units as the
    // origins above.
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

    // Latched, so it only has to be seen once and only the quads ahead of the first anchored element
    // on a page's very first frame go without.
    if (!bAnchorPage && bGameplayHudPage && IsAnchoredElement(fGroupX, fRight - fLeft))
    {
        RememberAnchorPage(pCurrentPage);
        bAnchorPage = true;
    }

    if (IsBackdrop(fRight - fLeft, fBottom - fTop))
    {
        Remap(pRender, nRenderVertexX, fBackdropOffsetX, fBackdropScaleX);
        Remap(pRender, nRenderVertexY, fBackdropOffsetY, fBackdropScaleY);
        return;
    }

    // Menu and dialog pages get the canvas, the backdrop remap and the hit test, and nothing else.
    if (!bGameplayHudPage || !bAnchorPage)
        return;

    const int nBandX = BandOf(fGroupX, static_cast<float>(nLeftZoneMax), static_cast<float>(nRightZoneMin));
    const int nBandY = BandOf(fGroupY, static_cast<float>(nTopZoneMax), static_cast<float>(nBottomZoneMin));

    const float fDeltaX = (nBandX < 0) ? fDeltaLeft : (nBandX > 0) ? fDeltaRight : 0.0f;

    float fDeltaY = 0.0f;
    if (nBandY < 0)
        fDeltaY = static_cast<float>(nShiftTop);
    else if (nBandY > 0)
        fDeltaY = static_cast<float>((nBandX > 0) ? nShiftBottomRight : nShiftBottom);

    Move(pRender, nRenderVertexX, fDeltaX);
    Move(pRender, nRenderVertexY, fDeltaY);
}

// ---------------------------------------------------------------------------------------------
// The cursor
// ---------------------------------------------------------------------------------------------

// 0x10AB5B7E. The cursor is not drawn by the page loop; FUN_10AB59A0 draws it afterwards in a block
// of its own, and ESI is the topmost page's magma::Package with EBP the display:
//
//     10ab5b40   MOV ECX,[EAX-0x8]        the topmost page
//     10ab5b48   CALL [vtable+0x20]       its package
//     10ab5b76   MOV [EBP+0x34],AX        canvas width
//     10ab5b7a   MOV [EBP+0x36],BX        canvas height
//     10ab5b95   MOV [EAX+0x38],BX        origin x, zeroed
//     10ab5b99   MOV [EAX+0x3a],BX        origin y, zeroed
//     10ab5ba8   CALL [vtable+0x24]       BeginScreen, then FUN_10AA21A0 per cursor slot
//
// Missing this block cost two things. The cursor skipped, because the block never reaches the canvas
// hook so the page flags were still set from the last page of the loop and the cursor's quad went
// through the HUD's band classification - it moves, so its origin crosses a boundary as it goes and
// the sprite jumps a few hundred units sideways and back. And its range was wrong, because
// FUN_104EFAD0 clamps the accumulated position against the same two shorts:
//
//     104efb92   MOVSX ..,[display+0x34]  canvas width, x clamped to 0..width
//     104efbc4   MOVSX ..,[display+0x36]  canvas height, y clamped to 0..height
//
// and this block is the last thing to write them each frame, so the cursor was clamped to the
// authored canvas while everything else drew against the corrected one. Splinter Cell Chaos Theory
// is the same library and its widescreen fix does the same thing one level up.
static void ApplyCursorCanvas(SafetyHookContext& regs)
{
    // Whatever this block draws, it is not a page of the loop, so nothing here may be shifted.
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
    if (!BuildPageCanvas(authored, fAspect, IsGameplayHudPackage(pPackage), fixed))
        return;

    // Width and height only. The origins this block writes are zero by design - the cursor lives in
    // raw canvas units - and that is as true of the corrected canvas as of the authored one.
    auto* pContext = reinterpret_cast<uint8_t*>(regs.ebp);
    *reinterpret_cast<int16_t*>(pContext + nContextCanvasWidth) = static_cast<int16_t>(fixed.nWidth);
    *reinterpret_cast<int16_t*>(pContext + nContextCanvasHeight) = static_cast<int16_t>(fixed.nHeight);
}

// ---------------------------------------------------------------------------------------------
// The click boxes
// ---------------------------------------------------------------------------------------------

// 0x10AA16C0. __thiscall on the page with the event behind it and RET 4, so __fastcall with the
// unused EDX in the middle is the same frame.
//
// magma hit tests in content units. FUN_10AB69F0 walks the display stack top down and for each page
// that takes input converts the event and compares it against the widget's own rect:
//
//     FUN_10AA16C0(&event)     event.x -= package originX, event.y -= package originY
//     FUN_10A97020(&rect, 0)   the widget's rect, in content units
//
// and FUN_10AA16C0 reads the origin off the Package through vtable +0x44 and +0x48, which is the
// authored one. Rendering uses the corrected one, so every hit box sat off by the difference: 22
// units left and 52 up at 16:9, and more as the frame gets wider.
static SafetyHookInline PageEventToContentHook{};

// The event's position is one dword at +0x04, x in the low short and y in the high one, which is how
// FUN_10AB69F0 reads it back to hit test.
static constexpr ptrdiff_t nEventPosition = 0x04;

static void __fastcall PageEventToContent(void* pPage, void* pEdx, uint8_t* pEvent)
{
    PageEventToContentHook.fastcall(pPage, pEdx, pEvent);

    if (pEvent == nullptr)
        return;

    auto* pPackage = reinterpret_cast<uint8_t*>(GetPagePackage(pPage));
    if (pPackage == nullptr)
        return;

    // Worked out from this page's own package rather than from whatever the last page drawn left
    // behind. The packages need not share an authored canvas, and input does not arrive between
    // pages in any order this could rely on.
    float fAspect = 0.0f;
    if (!GetFrameAspect(fAspect))
        return;

    const Canvas authored = ReadAuthoredCanvas(pPackage);

    Canvas fixed;
    if (!BuildPageCanvas(authored, fAspect, IsGameplayHudPackage(pPackage), fixed))
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
            // 0x1032D910. Both globals wildcarded; the shape is distinctive enough without them.
            auto displayPattern = dunia_pattern("33 C0 39 05 ? ? ? ? 74 09 A1 ? ? ? ? 83 C0 14 C3");

            // 0x105355B0. The prologue as far as the empty-string proxy, whose address is
            // wildcarded, plus the 0xF that follows and is the small-string capacity.
            auto lookupPattern = dunia_pattern(
                "81 EC 9C 00 00 00 53 56 57 8D 44 24 0F 50 8D 4C 24 34 33 DB BF ? ? ? ? BE 0F 00 00 00 51");

            // FUN_10AB59A0's per-page block, from the stashed origin y through both origin stores to
            // the load of magma::Render at 0x10AB5AEC, which is the hook site thirteen bytes in.
            auto beginScreenPattern = dunia_pattern("0F B7 4C 24 10 66 89 43 38 66 89 4B 3A 8B 0D");

            // FUN_105FAB60's tail, from the 1.0f load through the last two vertex writes to the
            // split-screen scale load at 0x105FAD67, eighteen bytes in. Hooking after the writes
            // rather than before the divide is what makes all four vertices readable, and
            // safetyhook's Context32 carries xmm0..xmm7 so a mid hook in the middle of this floating
            // point run restores what it interrupted.
            auto quadPattern = dunia_pattern(
                "F3 0F 10 0D ? ? ? ? F3 0F 11 46 4C F3 0F 11 56 54 F3 0F 10 96 F0 00 00 00");

            // FUN_10AB59A0's trailing cursor block, from the GetPackage vtable fetch through both
            // canvas stores to 0x10AB5B7E, which is seventeen bytes in. The pair of word stores to
            // EBP is what makes it this block and not the loop, which stores to EBX.
            auto cursorPattern = dunia_pattern(
                "8B 06 8B 50 3C 8B CE FF D2 66 89 45 34 66 89 5D 36 8B 0D");

            // 0x10AA16C0, the whole of it: GetPackage, then originX off vtable +0x44 subtracted from
            // the low short of the event's position dword and originY off +0x48 from the high one.
            auto eventPattern = dunia_pattern(
                "8B 01 8B 50 20 56 57 FF D2 8B 7C 24 0C 8B F0 8B 47 04 8B 16 89 44 24 0C 8B 42 44 8B CE FF D0");

            if (displayPattern.empty() || lookupPattern.empty() || beginScreenPattern.empty() || quadPattern.empty())
                return;

            GetDisplayDescriptor = reinterpret_cast<GetDisplayDescriptor_t>(displayPattern.get_first());

            GetPackageForPathHook = safetyhook::create_inline(lookupPattern.get_first(), GetPackageForPath);

            static auto BeginScreenHook = safetyhook::create_mid(beginScreenPattern.get_first(0x0D), ApplyCanvas);
            static auto QuadHook = safetyhook::create_mid(quadPattern.get_first(0x12), ShiftQuad);

            // Both independent of the two above and of each other, so each is installed on its own
            // and a miss costs only its own correction.
            if (!cursorPattern.empty())
            {
                static auto CursorCanvasHook = safetyhook::create_mid(cursorPattern.get_first(0x11), ApplyCursorCanvas);
            }

            if (!eventPattern.empty())
                PageEventToContentHook = safetyhook::create_inline(eventPattern.get_first(0), PageEventToContent);
        };
    }
} HudFixes;
