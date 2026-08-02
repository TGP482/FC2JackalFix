module;

#include <common.hxx>

export module guiduplicates;

import common;
import dunia;

// Changing the screen resolution from the main menu crashed the game. Original bug, and not the
// device reset itself: the reset tears the current GUI page down, and the teardown walks a widget
// that has already been freed.

static uintptr_t nModuleBase = 0;
static uintptr_t nModuleEnd = 0;

static void CacheModuleRange(HMODULE hModule)
{
    const auto nBase = reinterpret_cast<uintptr_t>(hModule);
    if (nBase == 0)
        return;

    const auto* pDos = reinterpret_cast<const IMAGE_DOS_HEADER*>(nBase);
    if (pDos->e_magic != IMAGE_DOS_SIGNATURE)
        return;

    const auto* pNt = reinterpret_cast<const IMAGE_NT_HEADERS*>(nBase + pDos->e_lfanew);
    if (pNt->Signature != IMAGE_NT_SIGNATURE)
        return;

    nModuleBase = nBase;
    nModuleEnd = nBase + pNt->OptionalHeader.SizeOfImage;
}

// VirtualQuery rather than a structured exception handler, so the check costs the same whether the
// page is there or not and the hook body stays an ordinary function.
static bool IsReadable(uintptr_t nAddress)
{
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(nAddress), &mbi, sizeof(mbi)) != sizeof(mbi))
        return false;

    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0)
        return false;

    const auto nRegionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return nAddress + sizeof(uintptr_t) <= nRegionEnd;
}

static bool IsLiveNode(const void* pNode)
{
    const auto nNode = reinterpret_cast<uintptr_t>(pNode);
    if (nNode == 0 || (nNode & 3) != 0 || nModuleBase == 0 || !IsReadable(nNode))
        return false;

    const auto nVtable = *reinterpret_cast<const uintptr_t*>(nNode);
    if (nVtable < nModuleBase || nVtable >= nModuleEnd || (nVtable & 3) != 0 || !IsReadable(nVtable))
        return false;

    // Slot zero as well. One extra read, and a freed block now has to hold a pointer into the
    // module that itself points into the module to get past this.
    const auto nSlot = *reinterpret_cast<const uintptr_t*>(nVtable);
    return nSlot >= nModuleBase && nSlot < nModuleEnd;
}

// __thiscall(manager, parent, duplicate), callee cleanup of eight bytes.
static SafetyHookInline DestroyDuplicateHook{};

static void __fastcall DestroyDuplicate(void* pManager, void* pEdx, void* pParent, void* pDuplicate)
{
    if (!IsLiveNode(pParent) || !IsLiveNode(pDuplicate))
        return;

    DestroyDuplicateHook.fastcall(pManager, pEdx, pParent, pDuplicate);
}

class GuiDuplicates
{
public:
    GuiDuplicates()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            CacheModuleRange(hDunia);
            if (nModuleBase == 0)
                return;

            // FUN_10AD40D0's loop body, from the vector bounds through the call at its end.
            // FUN_10AD3FB0 is reached through that call rather than by matching its own entry: the
            // entry is a stock SEH prologue four other functions in the module share byte for byte,
            // and the handler address inside it relocates.
            //
            //     8B 77 04     MOV ESI,[EDI+0x4]     vector begin
            //     3B 77 08     CMP ESI,[EDI+0x8]     vector end
            //     8B D9        MOV EBX,ECX
            //     74 28        JZ  0x10AD4109
            //     8B 06        MOV EAX,[ESI]         Pair*
            //     8B 48 04     MOV ECX,[EAX+0x4]     duplicate
            //     8B 10        MOV EDX,[EAX]         parent
            //     51 52        PUSH ECX / PUSH EDX
            //     8B CB        MOV ECX,EBX
            //     E8 ? ? ? ?   CALL FUN_10AD3FB0     <- the displacement read below, at +0x15
            auto destroyLoop = dunia_pattern("8B 77 04 3B 77 08 8B D9 74 28 8B 06 8B 48 04 8B 10 51 52 8B CB E8");
            if (destroyLoop.empty())
                return;

            auto* pCall = destroyLoop.get_first<uint8_t>(0x15);
            auto* pDestroyDuplicate = pCall + 5 + *reinterpret_cast<int32_t*>(pCall + 1);

            DestroyDuplicateHook = safetyhook::create_inline(pDestroyDuplicate, DestroyDuplicate);
        };
    }
} GuiDuplicates;
