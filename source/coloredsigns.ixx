/*
  "Removing coloured road signs" from An Almost Complete Guide to Far Cry 2 Modding, in code.

  The guide's version is a data edit: every MissionObjectiveSigns.* prototype in
  entitylibrarypatchoverride.fcb carries a "Colors" block (hash C512C6A9) with four Vector4s, None
  (DFA2AFF1), Main (1F1A625A), Subvert (7D65CCD1) and Underground (590E69F7). The fix rewrites the
  last three to match None, 0000803F x4, and needs a repacked patch.dat.

  Those four are the "Colors" property group of the CRoadSign component, registered in FUN_10667f30
  as children 0..3, all at offset 0x20 with indices 0..3. The shared accessor (FUN_10667860)
  computes object + [desc+0xC] + [desc+0x10] * 0x10, so the block is a flat Vector4[4] at
  CRoadSign+0x20 through +0x5F. The constructor (FUN_106679d0, vtable 0x10E8D988) seeds Colors[0]
  white and Colors[1..3] red before the entity library overwrites all four.

  Only FUN_10667b50(CRoadSign* this, int nTagIndex) reads them, reached from FUN_10667e10 when the
  sign's active objective tag changes and from the refresh thunk at 0x10667C80. It is a three-way
  select fed to CMaterialInstance::SetVector4 (FUN_10409E60) as material parameter 0x0E44F777:

    TEST ESI, ESI               ; nTagIndex
    JL   colour_none            ; no objective    -> Colors[0] at [EBP+0x20]
    CMP  ESI, [EBP+0x70]        ; MissionColors count
    JGE  colour_main            ; tag not in list -> Colors[1] at [EBP+0x30]
    ...                         ; else this + 0x20 + enumColor * 0x10, enumColor from [EBP+0x6C]

  Turning the JL into an unconditional JMP (0x7C -> 0xEB, same length, same target) sends every path
  to Colors[0], the untinted white entry both by the constructor's default and in the shipped data.

  ESI is deliberately left alone: the store two instructions past the call, MOV [EBP+0x10], ESI, is
  the component's CurrentTag bookkeeping. Clobbering it to -1 to force the existing JL would make
  FUN_10667e10 think the tag never settles and re-run this for every sign, every tick.

  Toggling live works, but signs already tinted keep their tint until their objective tag next
  changes. Starting or finishing any mission clears them.
*/

module;

#include <common.hxx>

export module coloredsigns;

import common;
import dunia;
import settings;

class ColoredSigns
{
public:
    ColoredSigns()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            // Tag load through the last of the three colour branches. The parameter hash push that
            // follows is the first absolute address, so it is left out.
            auto pattern = dunia_pattern("8B 74 24 18 85 F6 7C 1C 3B 75 70 7D 11 8B 45 6C 8B 0C B0 83 C1 02 C1 E1 04 03 CD 51 EB 0A 8D 55 30 52 EB 04 8D 45 20 50");
            if (pattern.empty())
                return;

            static raw_mem fnSignColorSelect(pattern.get_first(6), { 0xEB }); // JL colour_none -> JMP colour_none

            static auto ColoredSignsCB = []()
            {
                if (JackalFixSettings.GetInt(PREF_NOCOLOREDSIGNS) != 0)
                    fnSignColorSelect.Write();
                else
                    fnSignColorSelect.Restore();
            };

            ColoredSignsCB();

            JackalFix::onIniFileChange() += []()
            {
                ColoredSignsCB();
            };
        };
    }
} ColoredSigns;
