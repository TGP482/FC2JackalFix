/*
  Making the display settings take effect without a restart.

  Display Mode, Internal Resolution and Scaling Filter are all decided while the device is being
  built. The modules that own them, borderless and internalres, hook the present-parameter write,
  the window style and the backbuffer size, and every one of those hooks runs inside the engine's
  own mode change. Their settings follow the ini perfectly well; what they were missing was anything
  to make the engine perform a mode change once the game is up.

  That mechanism exists and is entirely the engine's:

    CFCXOptionDisplayPage's apply writes the new resolution into the render profile and raises the
    render settings broadcast (Dunia+3F8AB0). One of the observers on that broadcast belongs to the
    render manager and is two instructions long: MOV byte ptr [ECX+30h],1 / RET. That byte is the
    only thing it sets.

    The render manager's per-frame device tick (Dunia+34CA80) tests that byte, among others, and on
    it calls Dunia+354B30. That asks Dunia+33C7E0 whether the mode actually differs, and only if it
    says yes does it invoke the functor that ends in the renderer's SetMode, which rebuilds
    D3DPRESENT_PARAMETERS and flags the device for reset. The reset itself lands on the following
    tick, between frames, on whichever thread the engine owns the device from.

  So there are two things to do and no more: set the byte, and answer the "does it differ" question
  with yes once. Everything after that is the engine driving its own machinery on its own thread and
  at its own moment, which is the only way this is safe to do at all.

  Dunia+33C7E0 is not stubbed out. It is called and only its answer is overridden. It has side
  effects the mode change depends on: it measures the window, and it writes the size override at
  renderMgr+330h/+334h when the requested resolution does not fit the desktop.

  The forced answer is given once and only once. Dunia+33C7E0 has exactly two callers, Dunia+354B30
  and Dunia+358410, and they are the same three lines: ask, and on a yes run the SetMode functor. So
  a single forced yes cannot be spent on anything other than a mode change.
*/

module;

#include <common.hxx>

export module modereapply;

import common;
import dunia;
import settings;

// The render manager's "the profile moved" byte: what the broadcast observer sets, and what the
// device tick looks at before it asks anything else.
static constexpr ptrdiff_t nRenderManagerDirty = 0x30;

// push 0 / push 3B8h / call <alloc> / add esp,8 / test eax,eax / jz / mov ecx,eax /
// call <render manager ctor> / mov [<render manager>],eax. The manager's only construction.
static const char* const szRenderManagerPattern =
    "6A 00 68 B8 03 00 00 E8 ? ? ? ? 83 C4 08 85 C0 74 0D 8B C8 E8 ? ? ? ? A3 ? ? ? ?";
static constexpr ptrdiff_t nRenderManagerPointer = 27; // disp32 of mov [<render manager>],eax

// The head of Dunia+354B30: it loads the render profile, hands it to the comparison and then tests
// the dirty byte. Both of the things wanted here are in that one sequence.
static const char* const szModeCheckPattern = "53 56 57 8B 3D ? ? ? ? 57 8B F1 E8 ? ? ? ? 80 7E 30 00";
static constexpr ptrdiff_t nModeCheckCall = 12;       // call <does the mode differ>

static void** ppRenderManager = nullptr;

static SafetyHookInline ModeDiffersHook{};

// Raised when one of the settings below moves, lowered once the engine has actually gone through
// with the mode change. Read from the engine's threads, written from the file watcher's.
static std::atomic<bool> bModeChangePending = false;

// What was in force the last time the engine built its device, so a broadcast is only asked for
// when something that matters has really moved rather than on every APPLY.
static int32_t nAppliedDisplayMode = 0;
static int32_t nAppliedBaseW = 0;
static int32_t nAppliedBaseH = 0;
static int32_t nAppliedScalingFilter = 0;

// Both of this function's callers ask the same question and both act on the answer the same way: a
// yes runs the functor that ends in SetMode, and either way the dirty byte is cleared. So a single
// forced yes is spent on a real mode change and cannot be wasted anywhere else.
static uint8_t __fastcall ModeDiffers(void* pRenderManager, void* pEdx, void* pProfile)
{
    // Called rather than skipped: it measures the window and settles the size override the mode
    // change then uses. Only the answer is overridden.
    const auto nStock = ModeDiffersHook.fastcall<uint8_t>(pRenderManager, pEdx, pProfile);

    if (!bModeChangePending.exchange(false))
        return nStock;

    return 1;
}

// Asks the engine to rebuild its device at the next opportunity. Writing the byte is all this does:
// the tick that reads it belongs to the engine, and so does the thread it runs on.
static void RequestModeChange()
{
    if (ppRenderManager == nullptr)
        return;

    auto* pRenderManager = static_cast<uint8_t*>(*ppRenderManager);
    if (pRenderManager == nullptr)
    {
        return;
    }

    bModeChangePending = true;
    *(pRenderManager + nRenderManagerDirty) = 1;

}

class ModeReapply
{
public:
    ModeReapply()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            auto manager = dunia_pattern(szRenderManagerPattern);
            if (!manager.empty())
                ppRenderManager = *manager.get_first<void**>(nRenderManagerPointer);

            auto modeCheck = dunia_pattern(szModeCheckPattern);
            if (!modeCheck.empty())
            {
                auto* pCall = modeCheck.get_first<uint8_t>(nModeCheckCall);
                auto* pDiffers = pCall + 5 + *reinterpret_cast<int32_t*>(pCall + 1);
                ModeDiffersHook = safetyhook::create_inline(pDiffers, ModeDiffers);
            }

            static auto Remember = []()
            {
                nAppliedDisplayMode = JackalFixSettings.GetInt(PREF_DISPLAYMODE);
                nAppliedBaseW = JackalFixSettings.GetInt(PREF_INTERNALRESOLUTIONX);
                nAppliedBaseH = JackalFixSettings.GetInt(PREF_INTERNALRESOLUTIONY);
                nAppliedScalingFilter = JackalFixSettings.GetInt(PREF_SCALINGFILTER);
            };

            Remember();

            // Nothing here depends on running before or after borderless and internalres. The
            // rebuild is not performed now; it is asked for, and the engine gets to it on its next
            // tick, by which time every handler on this event has had its turn.
            JackalFix::onIniFileChange() += []()
            {
                const auto bMoved =
                    nAppliedDisplayMode != JackalFixSettings.GetInt(PREF_DISPLAYMODE)
                    || nAppliedBaseW != JackalFixSettings.GetInt(PREF_INTERNALRESOLUTIONX)
                    || nAppliedBaseH != JackalFixSettings.GetInt(PREF_INTERNALRESOLUTIONY)
                    || nAppliedScalingFilter != JackalFixSettings.GetInt(PREF_SCALINGFILTER);

                Remember();

                if (bMoved)
                    RequestModeChange();
            };
        };
    }
} ModeReapply;
