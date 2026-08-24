module;

#include <common.hxx>

export module guiduplicates;

import common;
import dunia;

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

// VirtualQuery rather than an SEH, so the hook body stays an ordinary function.
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

    // Slot zero too: a freed block must now hold a module pointer that itself points into the module.
    const auto nSlot = *reinterpret_cast<const uintptr_t*>(nVtable);
    return nSlot >= nModuleBase && nSlot < nModuleEnd;
}

// Changing resolution from the main menu crashed: the reset tears the current GUI page down and the
// teardown walks a widget that has already been freed.
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

            // Loop body of the duplicate-destroy pass; the CALL displacement at +0x15 gives the target.
            // Reached through the call, not its own entry: the SEH prologue there is shared by four
            // other functions and the handler address inside it relocates.
            if (auto* pCall = static_cast<uint8_t*>(dunia_find("8B 77 04 3B 77 08 8B D9 74 28 8B 06 8B 48 04 8B 10 51 52 8B CB E8", 0x15)))
            {
                auto* pDestroyDuplicate = pCall + 5 + *reinterpret_cast<int32_t*>(pCall + 1);

                DestroyDuplicateHook = safetyhook::create_inline(pDestroyDuplicate, DestroyDuplicate);
            }
        };
    }
} GuiDuplicates;
