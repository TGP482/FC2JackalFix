/*
  Credit to https://github.com/FoxAhead/Far-Cry-2-Multi-Fixer
  These fixes were implemented based on the Far Cry 2 Multi Fixer by FoxAhead.
*/

module;

#include <common.hxx>

export module bonuscontent;

import common;
import dunia;
import settings;

// Both unlocks are gated behind Ubisoft services that no longer exist, so the checks can never
// succeed. Force them.
class BonusContent
{
public:
    BonusContent()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            // Predecessor tape missions. The wrapper reports success only when the entitlement
            // lookup PrivilegesClient::GetPrivilege(1000) comes back positive.
            if (auto* pPredecessorTapes = dunia_find("8B 49 0C 85 C9 74 16 8B 44 24 04 50 E8 ? ? ? ? 84 C0 74 08 B8 01 00 00 00 C2 04 00", 5))
            {
                // JZ fail -> JMP success
                static raw_mem fnPredecessorTapes(pPredecessorTapes, { 0xEB, 0x0E });

                BindPatch(fnPredecessorTapes, PREF_PREDECESSORTAPES);
            }

            // Two extra machete skins, unlocked by the MachetesKey value a promotion wrote into
            // HKCU\Software\Ubisoft\Far Cry 2.
            if (auto* pMachetes = dunia_find("75 02 B3 01 8B 54 24 08 52 FF 15 ? ? ? ? 8A C3 5B 83 C4 10 C3", 15))
            {
                // MOV AL, BL -> MOV AL, 1
                static raw_mem fnMachetes(pMachetes, { 0xB0, 0x01 });

                BindPatch(fnMachetes, PREF_MACHETES);
            }
        };
    }
} BonusContent;
