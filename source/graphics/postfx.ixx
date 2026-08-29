module;

#include <common.hxx>
#include <atomic>

export module postfx;

import common;
import dunia;
import logging;
import settings;

static std::atomic<bool> bRemoveSprintAimBlur{ false };

// CScenePostFx::Render: __thiscall with four stack arguments, callee cleanup, so __fastcall
// matches. Returning 0 is the chain's "nothing drawn", and it keeps the source.
static SafetyHookInline BlurPassHook{};

static uint8_t __fastcall BlurPass(void* pPass, void* pEdx, void* pFrame, void* pScene,
    void* pSource, void* pTarget)
{
    if (bRemoveSprintAimBlur.load(std::memory_order_relaxed))
        return 0;

    return BlurPassHook.fastcall<uint8_t>(pPass, pEdx, pFrame, pScene, pSource, pTarget);
}

class PostFx
{
public:
    PostFx()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            // CScenePostFxBlur::Render, FUN_103E4BB0, first instruction. Carried through its
            // settings lookup for uniqueness; the absolute and call displacement are wildcarded.
            // Sprinting and aiming are the only things that reach it in normal play.
            auto* pBlur = dunia_find("55 8B EC 83 E4 F0 81 EC A4 00 00 00 8B 45 0C 53 56 57 8B D9 83 C0 08 50 B9 ? ? ? ? 89 5C 24 3C E8");
            if (!pBlur)
            {
                LogWarn("blur pass not found");
                return;
            }

            // Installed unconditionally so the toggle flips live.
            BlurPassHook = safetyhook::create_inline(pBlur, BlurPass);

            BindBool(bRemoveSprintAimBlur, PREF_NOSPRINTAIMBLUR);
        };
    }
} PostFx;
