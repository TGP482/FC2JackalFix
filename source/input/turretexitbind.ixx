module;

#include <common.hxx>

export module turretexitbind;

import common;
import dunia;

// common_using_mounted_weapon binds leave_mounted_weapon to kb:e and, unlike common_gameplay, does
// not import common_use_remap, so the turret stays on E after interact is rebound. A rebind is not
// a new binding: the options page writes a MassRename filter (from and to input pair) into
// common_use_remap in %APPDATA%InputUserActionMap.xml, and CActionMap import copies filters into
// the importing map.

// CRC-32 of the map name, held at CActionMap+0x6C.
static constexpr uintptr_t nMapNameId = 0x6C;
static constexpr uint32_t nCommonUsingMountedWeapon = 0xF103656E;
static constexpr uint32_t nCommonUseRemap = 0xD7BC2C30;

// Also the per mode map that imports it, since which of the two holds the binding when filters are
// applied depends on a document order that has not been pinned down.
static constexpr uint32_t nUsingMountedWeapon = 0x1FD171DD;

using FindActionMap_t = void*(__thiscall*)(void* pManager, const uint32_t* pNameId);

// Merges bindings, then filters. A MassRename meeting an existing one merges into it, so a repeat
// import of the same map is harmless.
using ImportActionMap_t = void(__thiscall*)(void* pMap, void* pImported);

static void** ppActionMapManager = nullptr;
static FindActionMap_t FindActionMap = nullptr;
static ImportActionMap_t ImportActionMap = nullptr;

class TurretExitBind
{
public:
    TurretExitBind()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            // The Import pass, at the lookup of the map the element names.
            //
            //     8B 0D 647B6411   MOV ECX,dword ptr [ActionMapManager]
            //     E8 E5080000      CALL FindByName
            //     3B C3            CMP EAX,EBX          <- hook, EAX is the map
            auto pattern = dunia_pattern("8D 4C 24 18 51 8B 0D ? ? ? ? E8 ? ? ? ? 3B C3 89 44 24 14 75 2A");
            if (pattern.empty())
                return;

            auto* pImportPass = reinterpret_cast<uint8_t*>(pattern.get_first(0));

            ppActionMapManager = *reinterpret_cast<void***>(pImportPass + 7);
            FindActionMap = reinterpret_cast<FindActionMap_t>(pImportPass + 16 + *reinterpret_cast<int32_t*>(pImportPass + 12));
            ImportActionMap = reinterpret_cast<ImportActionMap_t>(
                dunia_find("83 EC 44 8B 41 54 53 55 56 57 8B 7C 24 58 89 44 24 24 33 C0 39 47 54"));

            if (ImportActionMap == nullptr)
                return;

            static auto ImportPassHook = safetyhook::create_mid(pImportPass + 16, [](SafetyHookContext& regs)
            {
                auto* pMap = reinterpret_cast<uint8_t*>(regs.eax);
                auto* pManager = *ppActionMapManager;

                if (pMap == nullptr || pManager == nullptr)
                    return;

                auto nName = *reinterpret_cast<uint32_t*>(pMap + nMapNameId);
                if (nName != nCommonUsingMountedWeapon && nName != nUsingMountedWeapon)
                    return;

                // Absent until the player rebinds something, which is also when there is nothing to do.
                auto nRemap = nCommonUseRemap;
                if (auto* pRemap = FindActionMap(pManager, &nRemap))
                    ImportActionMap(pMap, pRemap);
            });
        };
    }
} TurretExitBind;
