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
            {
                auto pattern = dunia_pattern("8B 49 0C 85 C9 74 16 8B 44 24 04 50 E8 ? ? ? ? 84 C0 74 08 B8 01 00 00 00 C2 04 00");
                if (!pattern.empty())
                {
                    // JZ fail -> JMP success
                    static raw_mem fnPredecessorTapes(pattern.get_first(5), { 0xEB, 0x0E });

                    static auto PredecessorTapesCB = []()
                    {
                        if (JackalFixSettings.GetInt(PREF_PREDECESSORTAPES))
                            fnPredecessorTapes.Write();
                        else
                            fnPredecessorTapes.Restore();
                    };

                    PredecessorTapesCB();

                    JackalFix::onIniFileChange() += []()
                    {
                        PredecessorTapesCB();
                    };
                }
            }

            // Two extra machete skins, unlocked by the MachetesKey value a promotion wrote into
            // HKCU\Software\Ubisoft\Far Cry 2.
            {
                auto pattern = dunia_pattern("75 02 B3 01 8B 54 24 08 52 FF 15 ? ? ? ? 8A C3 5B 83 C4 10 C3");
                if (!pattern.empty())
                {
                    // MOV AL, BL -> MOV AL, 1
                    static raw_mem fnMachetes(pattern.get_first(15), { 0xB0, 0x01 });

                    static auto MachetesCB = []()
                    {
                        if (JackalFixSettings.GetInt(PREF_MACHETES))
                            fnMachetes.Write();
                        else
                            fnMachetes.Restore();
                    };

                    MachetesCB();

                    JackalFix::onIniFileChange() += []()
                    {
                        MachetesCB();
                    };
                }
            }
        };
    }
} BonusContent;
