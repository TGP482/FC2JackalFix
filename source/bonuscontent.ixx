/* Based on FoxAhead's Far Cry 2 Multi Fixer: https://github.com/FoxAhead/Far-Cry-2-Multi-Fixer */

module;

#include <common.hxx>

export module bonuscontent;

import common;
import dunia;
import settings;

// Both gate on dead Ubisoft services, so the checks never succeed.
class BonusContent
{
public:
    BonusContent()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            // Predecessor tapes: PrivilegesClient::GetPrivilege(1000).
            if (auto* pPredecessorTapes = dunia_find("8B 49 0C 85 C9 74 16 8B 44 24 04 50 E8 ? ? ? ? 84 C0 74 08 B8 01 00 00 00 C2 04 00", 5))
            {
                // JZ fail -> JMP success
                static raw_mem fnPredecessorTapes(pPredecessorTapes, { 0xEB, 0x0E });

                BindPatch(fnPredecessorTapes, PREF_PREDECESSORTAPES);
            }

            // Two extra machete skins, gated on MachetesKey in HKCU\Software\Ubisoft\Far Cry 2.
            if (auto* pMachetes = dunia_find("75 02 B3 01 8B 54 24 08 52 FF 15 ? ? ? ? 8A C3 5B 83 C4 10 C3", 15))
            {
                // MOV AL, BL -> MOV AL, 1
                static raw_mem fnMachetes(pMachetes, { 0xB0, 0x01 });

                BindPatch(fnMachetes, PREF_MACHETES);
            }
        };
    }
} BonusContent;
